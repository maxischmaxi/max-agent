#define _GNU_SOURCE // NOLINT(bugprone-reserved-identifier)
/* (ueberdeckt _POSIX_C_SOURCE: wir brauchen pthread_timedjoin_np
 * fuer die gnadenfrist beim abbruch nicht-killbarer tools) */

#include "send.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "chat.h"
#include "context.h"
#include "keys.h"
#include "prompt.h"
#include "session.h"
#include "tools.h"
#include "utils.h"

/* ChatToolCall und OaiToolCall sind absichtlich identisch aufgebaut
 * (chat.h), damit send_build_messages das array direkt borgen kann */
_Static_assert(sizeof(ChatToolCall) == sizeof(OaiToolCall),
               "ChatToolCall muss layout-kompatibel zu OaiToolCall bleiben");

/* timeout fuer nicht-streaming-anfragen. der npm-default sind 10
 * minuten, aber eine blockierende TUI, die 10 min nichts tut,
 * wirkt kaputt – lieber frueher abbrechen und im verlauf melden.
 * streaming hat ohnehin nur einen connect-timeout. */
#define SEND_TIMEOUT_MS 120000

/* redraw-drossel beim streaming: schnellere modelle liefern
 * hunderte chunks pro sekunde, ein frame pro chunk wuerde nur cpu
 * und terminal-bandbreite verbrennen. das erste fragment zeichnet
 * sofort (damit sofort text sichtbar wird), danach hoechstens
 * alle 80ms. */
#define SEND_REDRAW_MS 80

/* agent-loop: BEWUSST ohne runden-deckel, exakt wie der agent-loop
 * von pi (badlogic/pi-mono, agent-loop.ts): die schleife laeuft,
 * bis das modell keine tool-calls mehr ausgibt – ein fixer deckel
 * wuerde nur harmlose aufgaben abwuerzen, echte arbeiten brauchen
 * routinemaessig dutzende runden. die "grenzen" sind dieselben
 * wie dort: abbruch per esc/ctrl+c (stream_should_abort) und das
 * kontextfenster (ctx_trim_start kuemmt alten verlauf weg).
 * ein model, das sich in identischen calls todespiralt, ist ein
 * modell-problem (pi-issue #6158) – der benutzer bricht ab. */

/* deckel fuer parallel tool-calls in EINER antwort (schuetzt nur
 * den akku vor speicher-sprengung, kein verhaltens-limit) */
#define SEND_MAX_TOOLS 32

/* puffer fuer fehlermeldungen, die im transcript landen; laengere
 * server-texte werden abgeschnitten (chat_wrap umbricht eh). */
#define ERR_MAX 160

int send_role(ChatRole role)
{
    switch (role) {
    case CHAT_ROLE_SYSTEM:
        return OAI_ROLE_SYSTEM;
    case CHAT_ROLE_USER:
        return OAI_ROLE_USER;
    case CHAT_ROLE_ASSISTANT:
        return OAI_ROLE_ASSISTANT;
    case CHAT_ROLE_TOOL:
        return OAI_ROLE_TOOL;
    case CHAT_ROLE_ERROR:
    case CHAT_ROLE_NOTICE:
        return -1; /* lokale meldung: niemals an die api */
    }
    return -1;
}

int send_build_messages(const Chat *chat, const char *system_prompt,
                        OaiMessage **out)
{
    return send_build_messages_from(chat, 0, system_prompt, out);
}

int send_build_messages_from(const Chat *chat, size_t from,
                             const char *system_prompt, OaiMessage **out)
{
    if (chat == NULL || out == NULL) {
        return -1;
    }
    *out = NULL;
    if (from > chat->len) {
        from = chat->len;
    }

    bool has_system = false;
    if (system_prompt != NULL && system_prompt[0] != '\0') {
        has_system = true;
    }
    size_t n = 0;
    for (size_t i = from; i < chat->len; i++) {
        if (send_role(chat->msgs[i].role) >= 0) {
            n++;
        }
    }
    if (n == 0 && !has_system) {
        return 0;
    }
    if (has_system) {
        n++; /* system-prompt kommt als erste nachricht dazu */
    }

    OaiMessage *msgs = calloc(n, sizeof *msgs);
    if (msgs == NULL) {
        die("out of memory");
    }
    size_t j = 0;
    if (has_system) {
        msgs[j].role = OAI_ROLE_SYSTEM;
        msgs[j].content = system_prompt;
        j++;
    }
    for (size_t i = from; i < chat->len; i++) {
        int role = send_role(chat->msgs[i].role);
        if (role < 0) {
            continue; /* ERROR: nur gerendert, nie gesendet */
        }
        const ChatMessage *m = &chat->msgs[i];
        msgs[j].role = (OaiRole)role;
        msgs[j].content = m->text; /* const-borrow */

        if (role == OAI_ROLE_ASSISTANT && m->tool_calls_len > 0) {
            /* tool-calls mit zurueckgeben (round-trip der api):
             * ChatToolCall und OaiToolCall sind absichtlich gleich
             * aufgebaut (chat.h) – direkter cast reicht */
            if (m->text[0] == '\0') {
                msgs[j].content = NULL; /* nur tool-calls: content entfaellt */
            }
            msgs[j].tool_calls = (const OaiToolCall *)m->tool_calls;
            msgs[j].tool_calls_len = m->tool_calls_len;
        }
        if (role == OAI_ROLE_TOOL) {
            msgs[j].tool_call_id = m->tool_call_id;
        }
        j++;
    }
    *out = msgs;
    return (int)j;
}

const Model *send_find_model(const Config *cfg, const char *id,
                             const Provider **provider)
{
    if (cfg == NULL || id == NULL || id[0] == '\0') {
        return NULL;
    }
    for (size_t p = 0; p < cfg->providers_len; p++) {
        const Provider *pr = &cfg->providers[p];
        for (size_t m = 0; m < pr->models_len; m++) {
            if (pr->models[m].id != NULL && strcmp(pr->models[m].id, id) == 0) {
                if (provider != NULL) {
                    *provider = pr;
                }
                return &pr->models[m];
            }
        }
    }
    return NULL;
}

/* fehlermeldung als ERROR-nachricht in den verlauf; OOM ist hier
 * fatal (die app hat keinen sinnvollen weg weiter). http_status
 * landet nebenbei im session-log (0 = kein http-fehler). */
static void fail(AppState *state, long http_status, const char *fmt, ...)
{
    char buf[ERR_MAX];
    va_list ap;
    va_start(ap, fmt);
    (void)vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (chat_append(&state->chat, CHAT_ROLE_ERROR, buf) != 0) {
        die("out of memory");
    }
    (void)session_log_error(&state->session, buf, http_status);
}

/* wie fail(), aber als neutraler hinweis der app (NOTICE): geht
 * ebenfalls nie an die api, wird aber nicht rot gezeichnet */
static void notice(AppState *state, const char *fmt, ...)
{
    char buf[ERR_MAX];
    va_list ap;
    va_start(ap, fmt);
    (void)vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (chat_append(&state->chat, CHAT_ROLE_NOTICE, buf) != 0) {
        die("out of memory");
    }
    (void)session_log_notice(&state->session, buf);
}

/* ------------------------------------------------------------------ */
/* gemeinsames setup fuer send_message und send_stream: modell +     */
/* provider finden, system-prompt bauen, nachrichten bauen. fehler   */
/* landen im verlauf.                                                 */
/* ------------------------------------------------------------------ */
typedef struct {
    const Provider *provider;
    const Model *model;
    OaiMessage *msgs; /* geborgt aus dem chat (send_build_messages) */
    char *system;     /* heap-string aus prompt_build; req gehoert */
    int n;
    size_t est; /* geschaetzte prompt-tokens dieser runde: gegen  */
                /* OaiUsage gehalten, sobald die antwort da ist   */
} SendReq;

static int send_setup(AppState *state, const Config *cfg, SendReq *req)
{
    memset(req, 0, sizeof *req);

    const Provider *provider = NULL;
    const Model *model = send_find_model(cfg, cfg->active_model, &provider);
    if (model == NULL || provider == NULL) {
        fail(state, 0, "kein modell gewaehlt: /models");
        return -1;
    }
    if (provider->api_key == NULL || provider->api_key[0] == '\0') {
        fail(state, 0, "provider '%s' ohne api-key",
             (provider->base_url != NULL) ? provider->base_url : "?");
        return -1;
    }
    req->provider = provider;
    req->model = model;
    return 0;
}

/* msgs und system freigeben (beide nach dem request nicht mehr
 * noetig; msgs borgt content aus chat UND system) */
static void send_teardown(SendReq *req)
{
    free(req->msgs);
    req->msgs = NULL;
    free(req->system);
    req->system = NULL;
}

/* nachrichten fuer EINE runde bauen (der agent-loop ruft das mehr-
 * fach: das transcript waechst um assistant-tool-calls und tool-
 * ergebnisse). vorherige arrays werden freigegeben.
 *
 * hier faellt auch die entscheidung, wieviel verlauf ins fenster
 * des modells passt: context.c liefert den startindex, alles davor
 * sieht das modell nicht mehr. */
static int send_round_build(AppState *state, const Config *cfg, SendReq *req)
{
    Chat *chat = &state->chat;
    send_teardown(req);
    req->system = prompt_build(cfg); /* NULL = kein system-prompt */

    size_t budget = ctx_budget(req->model, req->system, &state->ctx);
    size_t from = ctx_trim_start(chat, budget);
    req->est = ctx_tokens_request(chat, from, req->system);

    /* nur melden, wenn diesmal MEHR wegfaellt als zuletzt – sonst
     * stuende in einem langen agent-loop nach jeder runde derselbe
     * hinweis im verlauf. der zaehler folgt aber IMMER dem aktuellen
     * stand, damit ein wechsel auf ein groesseres modell (und spaeter
     * zurueck) wieder gemeldet wird. */
    if (from > state->ctx.dropped) {
        notice(state,
               "verlauf gekuerzt: %zu %s am anfang weggelassen "
               "(budget ~%zu tokens)",
               from, (from == 1) ? "nachricht" : "nachrichten", budget);
    }
    state->ctx.dropped = from;

    int n = send_build_messages_from(chat, from, req->system, &req->msgs);
    if (n <= 0) {
        return -1; /* leerer verlauf: nichts zu senden */
    }
    req->n = n;
    return 0;
}

int send_message(AppState *state, const Config *cfg)
{
    if (state == NULL || cfg == NULL) {
        return -1;
    }
    Chat *chat = &state->chat;

    SendReq req;
    if (send_setup(state, cfg, &req) != 0 ||
        send_round_build(state, cfg, &req) != 0) {
        return -1;
    }

    OaiChatCompletionParams params = {
        .model = req.model->id,
        .messages = req.msgs,
        .messages_len = (size_t)req.n,
    };
    OaiClientOptions opts = {
        .api_key = req.provider->api_key,
        .base_url = req.provider->base_url,
        .timeout_ms = SEND_TIMEOUT_MS,
    };

    OaiClient client;
    if (oai_client_init(&client, &opts) != 0) {
        send_teardown(&req);
        fail(state, 0, "client-init fehlgeschlagen");
        return -1;
    }

    long long t_start = mono_ms();
    OaiCompletionResult result;
    int rc = oai_chat_completions_create(&client, &params, &result);
    oai_client_free(&client);
    send_teardown(&req);
    long long t_done = mono_ms();

    if (rc != 0) {
        fail(state, 0, "ungueltige anfrage-parameter");
        return -1;
    }
    if (!result.ok) {
        if (result.http_status > 0) {
            fail(state, result.http_status, "http %ld: %s", result.http_status,
                 (result.error != NULL) ? result.error : "?");
        } else {
            fail(state, 0, "verbindung: %s",
                 (result.error != NULL) ? result.error : "?");
        }
        oai_completion_result_free(&result);
        return -1;
    }

    /* antwort ins transcript (content NULL = nur tool-calls, ohne
     * tools nicht zu erwarten: dann leere nachricht) */
    const char *text = "";
    if (result.completion.choices_len > 0 &&
        result.completion.choices[0].message.content != NULL) {
        text = result.completion.choices[0].message.content;
    }
    if (result.completion.has_usage) {
        ctx_calibrate(&state->ctx, req.est,
                      result.completion.usage.prompt_tokens);
        ctx_account(&state->ctx, result.completion.usage.prompt_tokens,
                    result.completion.usage.completion_tokens);
    }
    if (chat_append(chat, CHAT_ROLE_ASSISTANT, text) != 0) {
        die("out of memory");
    }
    int ptok = -1;
    int ctok = -1;
    if (result.completion.has_usage) {
        ptok = result.completion.usage.prompt_tokens;
        ctok = result.completion.usage.completion_tokens;
    }
    (void)session_log_assistant(
        &state->session, text, NULL, 0, t_done - t_start, t_done - t_start,
        t_done - t_start, 0, (req.model->id != NULL) ? req.model->id : NULL,
        ptok, ctok, false);
    oai_completion_result_free(&result);
    return 0;
}

/* ------------------------------------------------------------------ */
/* tool-call-akku: streaming liefert tool-calls als deltas ueber den  */
/* index (id/name meist im ersten delta, arguments stueckweise).     */
/* hier waechst die finale call-liste zusammen, die am stream-ende an */
/* die platzhalter-nachricht gehaengt wird.                            */
/* ------------------------------------------------------------------ */
typedef struct {
    ChatToolCall *calls; /* slots bis len, zero-initialisiert */
    size_t len;
} ToolAcc;

/* string-append auf *dst (die() bei OOM, wie ueblich in der app) */
static void acc_str_append(char **dst, const char *add)
{
    size_t add_len = strlen(add);
    if (*dst == NULL) {
        *dst = malloc(add_len + 1);
        if (*dst == NULL) {
            die("out of memory");
        }
        memcpy(*dst, add, add_len + 1);
        return;
    }
    size_t old_len = strlen(*dst);
    char *grown = realloc(*dst, old_len + add_len + 1);
    if (grown == NULL) {
        die("out of memory");
    }
    memcpy(grown + old_len, add, add_len + 1);
    *dst = grown;
}

static void acc_add(ToolAcc *acc, const OaiChunkToolCall *d)
{
    if (d->index >= SEND_MAX_TOOLS) {
        return; /* deckel: mehr parallel-calls als erwartet */
    }
    if (d->index >= acc->len) {
        size_t new_len = d->index + 1;
        ChatToolCall *grown = realloc(acc->calls, new_len * sizeof *grown);
        if (grown == NULL) {
            die("out of memory");
        }
        memset(grown + acc->len, 0, (new_len - acc->len) * sizeof *grown);
        acc->calls = grown;
        acc->len = new_len;
    }
    ChatToolCall *c = &acc->calls[d->index];
    if (d->id != NULL && c->id == NULL) {
        c->id = dup_str(d->id);
        if (c->id == NULL) {
            die("out of memory");
        }
    }
    if (d->name != NULL && c->name == NULL) {
        c->name = dup_str(d->name);
        if (c->name == NULL) {
            die("out of memory");
        }
    }
    if (d->arguments_delta != NULL && d->arguments_delta[0] != '\0') {
        acc_str_append(&c->arguments, d->arguments_delta);
    }
}

static void acc_free(ToolAcc *acc)
{
    for (size_t i = 0; i < acc->len; i++) {
        free(acc->calls[i].id);
        free(acc->calls[i].name);
        free(acc->calls[i].arguments);
    }
    free(acc->calls);
    acc->calls = NULL;
    acc->len = 0;
}

/* slots ohne namen (kaputte modell-antwort) fallen lassen und das
 * rest-array dicht ruecken. acc->len wird mitgezogen, damit ein
 * spaeteres acc_free() die verworfenen zeiger nicht ein zweites mal
 * freigibt. */
static void acc_compact(ToolAcc *acc)
{
    size_t keep = 0;
    for (size_t i = 0; i < acc->len; i++) {
        if (acc->calls[i].name != NULL) {
            acc->calls[keep++] = acc->calls[i];
        } else {
            free(acc->calls[i].id);
            free(acc->calls[i].arguments);
        }
    }
    memset(acc->calls + keep, 0, (acc->len - keep) * sizeof *acc->calls);
    acc->len = keep;
}

/* ------------------------------------------------------------------ */
/* streaming: wie send_message, aber die antwort waechst live im      */
/* verlauf. der redraw-callback (gedrosselt) kommt aus keys.c.        */
/* ------------------------------------------------------------------ */

typedef struct {
    Chat *chat;
    Session *session;     /* NULL-sicher: inaktive session loggt nicht */
    const char *model;    /* id des modells dieser runde (fuer das log) */
    int round;            /* agent-loop-runde, 0-basiert */
    long long t_start;    /* runden-beginn (request abgeschickt) */
    long long t_first;    /* erster chunk; 0 = noch nichts an    */
    long long turn_start; /* beginn des GESAMTEN turns (busy_start);
                           * die arbeitszeit des turns laeuft ueber
                           * alle runden und tools hinweg */
    void *redraw_ctx;
    void (*redraw)(void *);
    long long last_ms;     /* zeit des letzten gedrosselten redraw */
    bool drew;             /* mindestens einmal gezeichnet */
    bool error;            /* on_error kam */
    ToolAcc acc;           /* tool-call-deltas waechsen hier zusammen */
    int prompt_tokens;     /* aus dem letzten chunk (include_usage): */
                           /* eichgroesse fuer ctx_calibrate, 0 = keine */
    int completion_tokens; /* dito, fuer die verbrauchs-anzeige */
    bool aborted;          /* benutzer hat esc/ctrl+c gedrueckt           */
} StreamCtx;

/* der watchdog des clients fragt das hier ~alle 100ms, auch waehrend
 * das modell noch denkt – der abbruch wartet also nicht auf den
 * naechsten chunk. */
static int stream_should_abort(void *ud)
{
    StreamCtx *sc = ud;
    if (sc->aborted) {
        return 1;
    }
    /* der watchdog laeuft ~alle 100ms – auch waehrend das modell
     * nur denkt und kein chunk fliesst. jede poll ist der frame-
     * tick, ohne den der spinner und die sekunden in der input-
     * rahmenzeile einfrieren wuerden */
    if (sc->redraw != NULL) {
        sc->redraw(sc->redraw_ctx);
    }
    if (keys_abort_pressed()) {
        sc->aborted = true;
        return 1;
    }
    return 0;
}

static int stream_on_chunk(const OaiChatCompletionChunk *chunk, void *ud)
{
    StreamCtx *sc = ud;

    /* erster datenverkehr aus dem modell: das ist die zeit, die es
     * zum nachdenken gebraucht hat (time to first token) */
    if (sc->t_first == 0) {
        sc->t_first = mono_ms();
    }

    for (size_t i = 0; i < chunk->choices_len; i++) {
        const char *delta = chunk->choices[i].content_delta;
        if (delta != NULL && !chat_append_text(sc->chat, delta)) {
            die("out of memory");
        }
        /* tool-call-deltas: gleiche logik wie beim text, nur dass
         * sie in den akku wandern statt in den platzhalter-text */
        for (size_t t = 0; t < chunk->choices[i].tool_call_deltas_len; t++) {
            acc_add(&sc->acc, &chunk->choices[i].tool_call_deltas[t]);
        }
    }
    if (chunk->has_usage) {
        sc->prompt_tokens = chunk->usage.prompt_tokens;
        sc->completion_tokens = chunk->usage.completion_tokens;
    }

    /* erster chunk sofort, danach hoechstens alle SEND_REDRAW_MS */
    long long now = mono_ms();
    if (!sc->drew || now - sc->last_ms >= SEND_REDRAW_MS) {
        sc->last_ms = now;
        sc->drew = true;
        sc->redraw(sc->redraw_ctx);
        /* an der drossel mitpruefen: waehrend die daten fliessen,
         * kaeme der watchdog erst nach bis zu einer sekunde. hier
         * kostet es einen select() pro frame. */
        if (stream_should_abort(sc)) {
            return 1;
        }
    }
    return 0;
}

static void stream_on_error(long http_status, const char *message, void *ud)
{
    StreamCtx *sc = ud;
    sc->error = true;

    /* platzhalter ohne inhalt wegwerfen; eine teil-antwort bleibt
     * im verlauf (der text verschwinden zu lassen waere schlimmer),
     * die fehlermeldung kommt als eigene nachricht hinterher */
    Chat *chat = sc->chat;
    if (chat->len > 0 &&
        chat->msgs[chat->len - 1].role == CHAT_ROLE_ASSISTANT &&
        chat->msgs[chat->len - 1].text[0] == '\0') {
        chat_pop(chat);
    } else if (chat->len > 0 &&
               chat->msgs[chat->len - 1].role == CHAT_ROLE_ASSISTANT) {
        /* teil-antwort bleibt im verlauf -> auch ins session-log,
         * sonst fehlt sie nach einem resume */
        (void)session_log_assistant(
            sc->session, chat->msgs[chat->len - 1].text, NULL, 0,
            (sc->t_first > 0) ? sc->t_first - sc->t_start : -1,
            mono_ms() - sc->t_start, mono_ms() - sc->turn_start, sc->round,
            sc->model, -1, -1, false);
    }
    char buf[ERR_MAX];
    if (http_status > 0) {
        (void)snprintf(buf, sizeof buf, "http %ld: %s", http_status,
                       (message != NULL) ? message : "?");
        (void)session_log_error(sc->session, buf, http_status);
    } else {
        (void)snprintf(buf, sizeof buf, "verbindung: %s",
                       (message != NULL) ? message : "?");
        (void)session_log_error(sc->session, buf, 0);
    }
    if (chat_append(chat, CHAT_ROLE_ERROR, buf) != 0) {
        die("out of memory");
    }
}

/* ------------------------------------------------------------------ */
/* asynchrone tool-ausfuehrung: das tool laeuft in einem worker-      */
/* thread, der haupt-thread haelt die ui am leben (spinner-tick) und  */
/* pollt fertig + abbruch. beim abbruch killt er das bash-kind samt   */
/* prozessgruppe (tool_kill_current) – ein haengendes kommando kann   */
/* die app damit NIE mehr blockieren, und ctrl+c wirkt auch hier.    */
/* ------------------------------------------------------------------ */
typedef struct {
    char *name; /* heap-kopien: ein abgekoppelter worker darf das */
    char *args; /* chat-objekt nicht mehr beruehren               */
    char *result;
    long long dur_ms; /* ausfuehrungsdauer, vom worker gemessen */
    atomic_bool done; /* worker fertig, result ist gueltig */
} ToolJob;

static void tool_job_free(ToolJob *job)
{
    free(job->name);
    free(job->args);
    free(job->result);
    free(job);
}

static void *tool_thread(void *ud)
{
    ToolJob *job = ud;
    long long t0 = mono_ms();
    job->result = tool_execute(job->name, job->args);
    job->dur_ms = mono_ms() - t0;
    atomic_store(&job->done, true);
    return NULL;
}

/* ein tool asynchron ausfuehren und dabei die ui am leben halten.
 * *dur_ms = ausfuehrungsdauer, *out_result = ergebnis (owned).
 * rueckgabe false = der benutzer hat abgebrochen (ergebnis verworfen
 * oder der worker abgekoppelt); OOM stirbt ehrlich. */
static bool tool_run_async(const ChatToolCall *call, const SendHooks *hooks,
                           char **out_result, long long *dur_ms)
{
    ToolJob *job = calloc(1, sizeof *job);
    if (job == NULL) {
        die("out of memory");
    }
    job->name = dup_str((call->name != NULL) ? call->name : "?");
    job->args = dup_str((call->arguments != NULL) ? call->arguments : "");
    if (job->name == NULL || job->args == NULL) {
        die("out of memory");
    }
    atomic_init(&job->done, false);

    pthread_t th;
    if (pthread_create(&th, NULL, tool_thread, job) != 0) {
        die("thread create failed"); /* ressourcen alle: ehrlich scheitern */
    }

    while (!atomic_load(&job->done)) {
        /* ui-tick: spinner und sekundenzaehler leben weiter, solange
         * das tool arbeitet */
        hooks->redraw(hooks->ctx);
        if (keys_abort_pressed()) {
            /* bash-kind killen (ganze prozessgruppe inkl. enkel-
             * kinder) – der worker kehrt danach sofort aus dem
             * read zurueck. DANN erst joinen: ein join vorher
             * wuerde auf das haengende kommando warten */
            tool_kill_current();

            /* gnadenfrist: killbare tools (bash) enden sofort.
             * nicht-killbare (read_file auf einem haengenden fs)
             * bekommen zwei sekunden, danach werden sie abgekoppelt
             * – die app blockiert NIE. der worker raeumt sein job
             * selbst ab (heap, strings sind kopiert). */
            struct timespec grace = {2, 0};
            if (pthread_timedjoin_np(th, NULL, &grace) == 0) {
                tool_job_free(job);
            } else {
                (void)pthread_detach(th);
            }
            return false;
        }
        /* 50ms: fluessige animation, kaum cpu */
        struct timespec ts = {0, 50L * 1000L * 1000L};
        (void)nanosleep(&ts, NULL);
    }

    (void)pthread_join(th, NULL);
    *dur_ms = job->dur_ms;
    *out_result = job->result;
    job->result = NULL; /* eigentum an den aufrufer */
    free(job->name);
    free(job->args);
    free(job);
    return true;
}

int send_stream(AppState *state, const Config *cfg, const SendHooks *hooks)
{
    if (state == NULL || cfg == NULL || hooks == NULL ||
        hooks->redraw == NULL) {
        return -1;
    }
    Chat *chat = &state->chat;

    SendReq req;
    if (send_setup(state, cfg, &req) != 0) {
        return -1;
    }

    /* tool-registry: die definitionen gehen in jede anfrage; ohne
     * tool_choice-feld entspricht das "auto" */
    size_t tools_len = 0;
    const OaiTool *tools = tool_registry(&tools_len);

    /* beginn des gesamt-turns: keys.c setzt busy_start_ms beim
     * abschicken; ohne das (tests rufen send_stream direkt) zaehlt
     * ab hier */
    long long turn_start =
        (state->busy_start_ms > 0) ? state->busy_start_ms : mono_ms();

    for (int round = 0;; round++) {
        if (send_round_build(state, cfg, &req) != 0) {
            send_teardown(&req);
            return -1;
        }

        /* platzhalter ERST NACH dem message-bau: send_build_messages
         * borgt content-pointer aus dem chat, und waehrend des streams
         * reallociert nur der platzhalter-text. das msgs-array bewegt
         * sich beim anhaengen nicht – die geborgten pointer bleiben
         * unberuehrt gueltig */
        if (chat_append(chat, CHAT_ROLE_ASSISTANT, "") != 0) {
            die("out of memory");
        }

        OaiChatCompletionParams params = {
            .model = req.model->id,
            .messages = req.msgs,
            .messages_len = (size_t)req.n,
            .tools = tools,
            .tools_len = tools_len,
            .include_usage = true, /* letzter chunk: token-zaehlung */
        };
        OaiClientOptions opts = {
            .api_key = req.provider->api_key,
            .base_url = req.provider->base_url,
            .timeout_ms = SEND_TIMEOUT_MS,
        };

        OaiClient client;
        if (oai_client_init(&client, &opts) != 0) {
            send_teardown(&req);
            chat_pop(chat); /* platzhalter wieder weg */
            fail(state, 0, "client-init fehlgeschlagen");
            return -1;
        }

        StreamCtx sc = {
            .chat = chat,
            .session = &state->session,
            .model = (req.model->id != NULL) ? req.model->id : NULL,
            .round = round,
            .t_start = mono_ms(),
            .turn_start = turn_start,
            .redraw_ctx = hooks->ctx,
            .redraw = hooks->redraw,
        };
        OaiStreamCallbacks cbs = {
            .on_chunk = stream_on_chunk,
            .on_error = stream_on_error,
            .should_abort = stream_should_abort,
            .user_data = &sc,
        };

        int rc = oai_chat_completions_create_stream(&client, &params, &cbs);
        oai_client_free(&client);
        send_teardown(&req);

        if (rc != 0) {
            acc_free(&sc.acc);
            chat_pop(chat); /* platzhalter wieder weg */
            fail(state, 0, "ungueltige anfrage-parameter");
            return -1;
        }
        if (sc.error) {
            acc_free(&sc.acc);
            return -1; /* die fehlermeldung steht als ERROR-nachricht */
        } /* im verlauf – mehr gibt es hier nicht zu tun */

        /* abbruch durch den benutzer: angefangene tool-calls werden
         * NICHT ausgefuehrt (er wollte ja gerade, dass nichts mehr
         * passiert). eine teil-antwort bleibt stehen, ein leerer
         * platzhalter geht weg. */
        if (sc.aborted) {
            acc_free(&sc.acc);
            bool partial = false;
            if (chat->len > 0 && chat->msgs[chat->len - 1].text[0] != '\0' &&
                chat->msgs[chat->len - 1].role == CHAT_ROLE_ASSISTANT) {
                partial = true;
            } else if (chat->len > 0 &&
                       chat->msgs[chat->len - 1].text[0] == '\0' &&
                       chat->msgs[chat->len - 1].role == CHAT_ROLE_ASSISTANT) {
                chat_pop(chat);
            }
            if (partial) {
                (void)session_log_assistant(
                    &state->session, chat->msgs[chat->len - 1].text, NULL, 0,
                    (sc.t_first > 0) ? sc.t_first - sc.t_start : -1,
                    mono_ms() - sc.t_start, mono_ms() - sc.turn_start, round,
                    sc.model, -1, -1, true);
            }
            notice(state, "abgebrochen");
            return 0;
        }

        /* schaetzung gegen die api-zaehlung halten: die naechste
         * runde rechnet mit dem korrigierten faktor */
        ctx_calibrate(&state->ctx, req.est, sc.prompt_tokens);
        ctx_account(&state->ctx, sc.prompt_tokens, sc.completion_tokens);

        /* akku uebernehmen: die letzte nachricht bekommt die calls.
         * chat_set_tool_calls uebernimmt das array (die() bei OOM
         * waere ok, aber die funktion gibt sauber -1) */
        if (sc.acc.len > 0) {
            acc_compact(&sc.acc);
            if (sc.acc.len > 0) {
                if (chat_set_tool_calls(chat, sc.acc.calls, sc.acc.len) != 0) {
                    /* alle felder per acc_free freigeben */
                    acc_free(&sc.acc);
                    die("out of memory");
                }
                sc.acc.calls = NULL; /* ownership im chat */
                sc.acc.len = 0;
            } else {
                acc_free(&sc.acc); /* nichts brauchbares angekommen */
            }
        }

        /* agent-loop: keine tool-calls -> fertig, antwort steht.
         * erst hier weiss die antwort endgueltig, was sie ist –
         * jetzt (und nur bei erfolg) wandert sie ins session-log,
         * mit allen messwerten der runde. */
        ChatMessage *last = &chat->msgs[chat->len - 1];
        if (last->tool_calls_len == 0) {
            (void)session_log_assistant(
                &state->session, last->text, NULL, 0,
                (sc.t_first > 0) ? sc.t_first - sc.t_start : -1,
                mono_ms() - sc.t_start, mono_ms() - sc.turn_start, round,
                sc.model, sc.prompt_tokens, sc.completion_tokens, false);
            return 0;
        }

        /* antwort MIT tool-calls: calls haengen an der nachricht, die
         * gleich ausgefuehrt werden – als eine zeile mitspeichern */
        (void)session_log_assistant(
            &state->session, last->text, last->tool_calls, last->tool_calls_len,
            (sc.t_first > 0) ? sc.t_first - sc.t_start : -1,
            mono_ms() - sc.t_start, mono_ms() - sc.turn_start, round, sc.model,
            sc.prompt_tokens, sc.completion_tokens, false);

        /* tools ausfuehren, ergebnisse als TOOL-nachrichten anhaengen;
         * die naechste runde schickt sie mit. fehler-ergebnisse sind
         * auch nur text – das model darf sie korrigieren. */
        /* die call-liste VOR der schleife sichern: chat_append_tool
         * laesst das msgs-array wachsen und verschiebt es dabei –
         * `last` zeigt danach ins freigegebene. das tool_calls-array
         * selbst gehoert der nachricht und bleibt liegen. */
        ChatToolCall *calls = last->tool_calls;
        size_t calls_len = last->tool_calls_len;
        for (size_t i = 0; i < calls_len; i++) {
            ChatToolCall *call = &calls[i];

            /* asynchron: die ui bleibt lebendig, und der abbruch
             * killt das tool (bash: ganze prozessgruppe). die calls
             * laufen bewusst weiter sequenziell – ihre ergebnisse
             * haengen in der regel voneinander ab */
            long long dur = -1;
            char *result = NULL;
            if (!tool_run_async(call, hooks, &result, &dur)) {
                /* abbruch: angefangene calls bleiben ohne antwort
                 * (wie beim stream-abbruch), die runde endet hier */
                notice(state, "abgebrochen");
                return 0;
            }
            if (result == NULL) {
                die("out of memory");
            }
            (void)session_log_tool(&state->session, call->id, call->name,
                                   result, dur);
            if (chat_append_tool(chat, call->id, result) != 0) {
                die("out of memory");
            }
            free(result);
        }
    }
}
