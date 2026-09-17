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
#include "debug.h"
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
    return send_build_messages_from(chat, 0, system_prompt, NULL, out);
}

int send_build_messages_from(const Chat *chat, size_t from,
                             const char *system_prompt, const char *summary,
                             OaiMessage **out)
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
    /* summary_wrapped (NULL = keine): BEREITS verpackte compaction-
     * summary (ctx_summary_wrap) – der aufrufer besitzt den string,
     * das array borgt ihn nur, wie system_prompt auch */
    bool has_summary = (bool)(summary != NULL && summary[0] != '\0');
    size_t n = 0;
    for (size_t i = from; i < chat->len; i++) {
        if (send_role(chat->msgs[i].role) >= 0) {
            n++;
        }
    }
    if (n == 0 && !has_system && !has_summary) {
        return 0;
    }
    if (has_system) {
        n++; /* system-prompt kommt als erste nachricht dazu */
    }
    if (has_summary) {
        n++; /* summary direkt dahinter */
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
    if (has_summary) {
        msgs[j].role = OAI_ROLE_USER;
        msgs[j].content = summary;
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
    OaiMessage *msgs;      /* geborgt aus dem chat (send_build_messages) */
    char *system;          /* heap-string aus prompt_build; req gehoert */
    char *summary_wrapped; /* compaction-summary, eingepackt (NULL =
                            * keine); gehoert dem req, nicht dem chat */
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

/* msgs, system und summary freigeben (alle nach dem request nicht
 * mehr noetig; msgs borgt content aus chat, system UND summary) */
static void send_teardown(SendReq *req)
{
    free(req->msgs);
    req->msgs = NULL;
    free(req->system);
    req->system = NULL;
    free(req->summary_wrapped);
    req->summary_wrapped = NULL;
}

/* ------------------------------------------------------------------ */
/* compaction: statt verlauf still vom anfang wegzukuerzen, wird    */
/* alles bis zum cut-punkt per llm zu einer strukturierten summary  */
/* verdichtet (ziel: aufgabenkontext ueberleben lassen). modell und */
/* format folgen dem pi-agenten (compaction.js): fester            */
/* zielrahmen (## Goal / ## Progress / ...), update-variante mit    */
/* <previous-summary>, und ein grosszueges keep-fenster, damit      */
/* compaction selten noetig wird. scheitert der zusammenfassungs-    */
/* call, ist trimming immer noch die fallback (altes verhalten).    */
/* ------------------------------------------------------------------ */

/* wieviel verlauf pro compaction UNVERKUERZT bleibt (pi: 20k). der
 * rest des fensters ist luft, die die naechste compaction
 * hinauszögert. */
#define COMPACT_KEEP_TOKENS 20000

/* nach einem gescheiterten compaction-versuch erst wieder neu
 * ansetzen, wenn wieder soviel verlauf gefallen ist */
#define COMPACT_RETRY_MIN_TOKENS 4096

/* deckel fuer die summary-ausgabe: genug fuer den rahmen, aber die
 * compaction soll keine halbe seite werden (pi: min(80% reserve,
 * model-max) ≈ 13k; wir sind sparsamer) */
#define COMPACT_MAX_TOKENS 8192

static const char COMPACT_SYSTEM[] =
    "You are a context summarization assistant. Read the conversation "
    "between a user and an AI coding agent, then produce a structured "
    "summary in the exact format specified. Do NOT continue the "
    "conversation. Do NOT respond to questions in it. ONLY output the "
    "structured summary.";

static const char COMPACT_FORMAT[] =
    "Use this EXACT format:\n\n"
    "## Goal\n[What is the user trying to accomplish?]\n\n"
    "## Constraints & Preferences\n"
    "- [Any constraints, preferences or requirements mentioned by the "
    "user, or \"(none)\"]\n\n"
    "## Progress\n"
    "### Done\n- [x] [Completed tasks/changes]\n"
    "### In Progress\n- [ ] [Current work]\n"
    "### Blocked\n- [Issues preventing progress, if any]\n\n"
    "## Key Decisions\n- **[Decision]**: [Brief rationale]\n\n"
    "## Next Steps\n1. [Ordered list of what should happen next]\n\n"
    "## Critical Context\n"
    "- [Any data, file paths, function names, error messages or "
    "references needed to continue]\n\n"
    "Keep each section concise. Preserve exact file paths, function "
    "names, and error messages.";

/* stream-kontext der compaction: sammelt die summary ein, haelt die
 * ui am leben und merkt sich abbruch/fehler. an den chat wird NICHT
 * angefasst – compaction ist kein chat-ereignis. */
typedef struct {
    char *buf; /* die summary waechst hier herein */
    size_t len;
    size_t cap;
    void *ui_ctx;
    void (*tick)(void *);
    void (*redraw)(void *);
    bool drew;
    bool error;
    bool aborted;
} CompactCtx;

static int compact_should_abort(void *ud)
{
    CompactCtx *cc = ud;
    if (cc->aborted) {
        return 1;
    }
    if (cc->tick != NULL) {
        cc->tick(cc->ui_ctx);
    } else if (cc->redraw != NULL) {
        cc->redraw(cc->ui_ctx);
    }
    if (keys_abort_pressed()) {
        cc->aborted = true;
        return 1;
    }
    return 0;
}

static int compact_on_chunk(const OaiChatCompletionChunk *chunk, void *ud)
{
    CompactCtx *cc = ud;
    for (size_t i = 0; i < chunk->choices_len; i++) {
        const char *delta = chunk->choices[i].content_delta;
        if (delta == NULL) {
            continue;
        }
        size_t add = strlen(delta);
        if (cc->len + add + 1 > cc->cap) {
            size_t ncap = (cc->cap == 0) ? 512 : cc->cap * 2;
            while (ncap < cc->len + add + 1) {
                ncap *= 2;
            }
            char *grown = realloc(cc->buf, ncap);
            if (grown == NULL) {
                die("out of memory");
            }
            cc->buf = grown;
            cc->cap = ncap;
        }
        memcpy(cc->buf + cc->len, delta, add);
        cc->len += add;
        cc->buf[cc->len] = '\0';
    }
    return 0;
}

static void compact_on_error(long http_status, const char *message, void *ud)
{
    CompactCtx *cc = ud;
    cc->error = true;
    (void)http_status;
    dbg("compaction: fehler http=%ld msg=%s", http_status,
        (message != NULL) ? message : "?");
}

/* verlauf [0, cut) als text serialisieren: die summary braucht die
 * inhalte, nicht das api-format. tool-calls + ergebnisse gehoeren
 * zur arbeit des agenten und muessen mit. lokale rollen (ERROR,
 * NOTICE) ueberspringen – die api sieht die auch nie. heap-string. */
static char *compact_serialize(const Chat *chat, size_t cut)
{
    size_t cap = 8192;
    size_t len = 0;
    char *buf = malloc(cap);
    if (buf == NULL) {
        die("out of memory");
    }

    for (size_t i = 0; i < cut; i++) {
        const ChatMessage *m = &chat->msgs[i];
        if (!chat_role_sent(m->role)) {
            continue;
        }
        /* rolle + text + zwei newlines; tool-calls als eigene zeilen */
        const char *role = NULL;
        if (m->role == CHAT_ROLE_USER) {
            role = "user";
        } else if (m->role == CHAT_ROLE_ASSISTANT) {
            role = "assistant";
        } else if (m->role == CHAT_ROLE_TOOL) {
            role = "tool result";
        }
        if (role == NULL) {
            continue; /* system steht nicht im transcript */
        }
        size_t need = strlen(role) + 2 + strlen(m->text) + 2;
        if (len + need + 1 > cap) {
            while (cap < len + need + 1) {
                cap *= 2;
            }
            char *grown = realloc(buf, cap);
            if (grown == NULL) {
                die("out of memory");
            }
            buf = grown;
        }
        len +=
            (size_t)snprintf(buf + len, cap - len, "%s: %s\n\n", role, m->text);
        for (size_t t = 0; t < m->tool_calls_len; t++) {
            const ChatToolCall *c = &m->tool_calls[t];
            const char *args = (c->arguments != NULL) ? c->arguments : "";
            need = 32 + strlen(args);
            if (len + need + 1 > cap) {
                while (cap < len + need + 1) {
                    cap *= 2;
                }
                char *grown = realloc(buf, cap);
                if (grown == NULL) {
                    die("out of memory");
                }
                buf = grown;
            }
            len += (size_t)snprintf(buf + len, cap - len,
                                    "assistant tool call: %s(%s)\n\n",
                                    (c->name != NULL) ? c->name : "?", args);
        }
    }
    if (len == 0) {
        buf[0] = '\0'; /* leere serialisierung: leerer prompt-rest */
    }
    return buf;
}

/* den verlauf [0, cut) (plus ggf. der bisherigen summary) per llm
 * verdichten. der client gehoert dem aufrufer (turn-weit, send.c
 * oben) – compaction ist nur ein request mehr auf derselben
 * verbindung. rueckgabe: 0 = summary steht (ctx->summary/covered
 * aktualisiert), -1 = fehlgeschlagen (fallback: trimming), 1 =
 * benutzer-abbruch (turn soll enden). */
static int compact_run(AppState *state, const SendReq *req, OaiClient *client,
                       size_t cut, const SendHooks *hooks)
{
    Chat *chat = &state->chat;
    CtxUsage *ctx = &state->ctx;
    bool update = (ctx->summary != NULL);

    /* input: alte summary + serialisierter verlauf + arbeitsauftrag */
    char *ser = compact_serialize(chat, cut);
    size_t plen = 0;
    if (update) {
        plen = strlen(ctx->summary) + 64;
    }
    size_t ilen = plen + strlen(ser) + strlen(COMPACT_FORMAT) + 1024;
    char *input = malloc(ilen);
    if (input == NULL) {
        free(ser);
        die("out of memory");
    }
    size_t off = 0;
    if (update) {
        off += (size_t)snprintf(
            input + off, ilen - off,
            "<previous-summary>\n%s\n</previous-summary>\n\n", ctx->summary);
    }
    off += (size_t)snprintf(input + off, ilen - off, "%s", ser);
    free(ser);
    if (update) {
        (void)snprintf(
            input + off, ilen - off,
            "\n\nThe messages above are NEW conversation messages to "
            "incorporate into the existing summary provided in "
            "<previous-summary> tags. PRESERVE all existing "
            "information, ADD new progress, decisions and context, "
            "move finished items from \"In Progress\" to \"Done\", and "
            "update \"Next Steps\".\n\n%s",
            COMPACT_FORMAT);
    } else {
        (void)snprintf(
            input + off, ilen - off,
            "\n\nThe messages above are the conversation to summarize. "
            "Create a structured context checkpoint that another LLM "
            "will use to continue the work.\n\n%s",
            COMPACT_FORMAT);
    }

    notice(state, "verlauf wird komprimiert (%zu %s)...", cut,
           (cut == 1) ? "nachricht" : "nachrichten");
    if (hooks != NULL && hooks->redraw != NULL) {
        hooks->redraw(hooks->ctx); /* hinweis sofort sichtbar */
    }

    OaiMessage msgs[2] = {
        {.role = OAI_ROLE_SYSTEM, .content = COMPACT_SYSTEM},
        {.role = OAI_ROLE_USER, .content = input},
    };
    OaiChatCompletionParams params = {
        .model = req->model->id,
        .messages = msgs,
        .messages_len = 2,
        .has_max_tokens = true,
        .max_tokens = COMPACT_MAX_TOKENS,
    };
    CompactCtx cc = {
        .ui_ctx = (hooks != NULL) ? hooks->ctx : NULL,
        .tick = (hooks != NULL) ? hooks->tick : NULL,
        .redraw = (hooks != NULL) ? hooks->redraw : NULL,
    };
    OaiStreamCallbacks cbs = {
        .on_chunk = compact_on_chunk,
        .on_error = compact_on_error,
        .should_abort = compact_should_abort,
        .user_data = &cc,
    };

    int rc = oai_chat_completions_create_stream(client, &params, &cbs);
    free(input);

    if (cc.aborted) {
        free(cc.buf);
        return 1;
    }
    if (rc != 0 || cc.error || cc.buf == NULL || cc.len == 0) {
        free(cc.buf);
        return -1;
    }

    /* summary uebernehmen: alte freigeben, neue samt watermark setzen */
    free(ctx->summary);
    ctx->summary = cc.buf;
    ctx->covered = cut;
    ctx->compact_failed = false;
    (void)session_log_compaction(&state->session, ctx->summary, cut);
    notice(state,
           "verlauf komprimiert: %zu %s zu einer zusammenfassung "
           "verdichtet (%.1f KB)",
           cut, (cut == 1) ? "nachricht" : "nachrichten",
           (double)strlen(cc.buf) / 1024.0);
    dbg("compaction: ok (%zu bytes, covered=%zu)", cc.len, cut);
    return 0;
}

/* nachrichten fuer EINE runde bauen (der agent-loop ruft das mehr-
 * fach: das transcript waechst um assistant-tool-calls und tool-
 * ergebnisse). vorherige arrays werden freigegeben.
 *
 * hier faellt auch die entscheidung, wieviel verlauf ins fenster
 * des modells passt: context.c liefert den startindex, alles davor
 * sieht das modell nicht mehr. laeuft das fenster ueber, wird der
 * abfallende verlauf VOR dem kuerzen per compaction (llm-call)
 * verdichtet, statt ihn still verschwinden zu lassen – hooks
 * haelt dabei die ui am leben (NULL = ohne, send_message). der
 * client gehoert dem turn: eine verbindung fuer alle runden und
 * die compaction darauf (ein tls-handshake pro runde waere rein
 * verschenktes warten). */
static int send_round_build(AppState *state, const Config *cfg, SendReq *req,
                            OaiClient *client, const SendHooks *hooks)
{
    Chat *chat = &state->chat;
    CtxUsage *ctx = &state->ctx;
    send_teardown(req);
    req->system = prompt_build(cfg); /* NULL = kein system-prompt */

    size_t budget = ctx_budget(req->model, req->system, ctx->summary, ctx);
    size_t from = ctx_trim_start(chat, budget);
    /* summary-watermark: was schon verdichtet ist, wird nicht nochmal
     * geschickt – der request bleibt klein, bis das fenster wieder
     * laeuft (nur mit summary: ohne waere das ein stiller verlust) */
    if (ctx->summary != NULL && from < ctx->covered) {
        from = ctx->covered;
    }

    /* verlauf faellt, den die summary noch nicht deckt -> compaction.
     * nach einem fehlversuch erst wieder, wenn genug neu gefallen ist
     * (sonst pro runde ein teurer llm-versuch, der eh scheitert). */
    if (from > ctx->covered) {
        size_t fresh = 0;
        for (size_t i = ctx->covered; i < from; i++) {
            fresh += ctx_tokens_message(&chat->msgs[i]);
        }
        if (!ctx->compact_failed || fresh >= COMPACT_RETRY_MIN_TOKENS) {
            size_t keep = COMPACT_KEEP_TOKENS;
            if (budget != CTX_NO_LIMIT && keep > budget / 2) {
                keep = budget / 2;
            }
            size_t cut = ctx_cut_point(chat, from, keep);
            int rc = compact_run(state, req, client, cut, hooks);
            if (rc == 1) {
                notice(state, "abgebrochen");
                return -1; /* benutzer-abbruch: turn beenden */
            }
            if (rc == 0) {
                /* budget neu rechnen: die summary ist ab jetzt fixer
                 * bestandteil jedes requests, der trim-punkt rutscht
                 * entsprechend – compaction verkleinert den request
                 * drastisch, bis das fenster wieder laeuft */
                budget = ctx_budget(req->model, req->system, ctx->summary, ctx);
                from = ctx_trim_start(chat, budget);
                if (from < ctx->covered) {
                    from = ctx->covered;
                }
            } else {
                ctx->compact_failed = true;
                ctx->covered = from; /* fallback: trimming, alt bekannt */
                notice(state,
                       "verlauf gekuerzt: %zu %s am anfang weggelassen "
                       "(compaction fehlgeschlagen, budget ~%zu tokens)",
                       from, (from == 1) ? "nachricht" : "nachrichten", budget);
            }
        } else {
            /* drossel: noch nicht genug neues gefallen, um es erneut
             * zu versuchen – still kuerzen wie vorher */
            ctx->covered = from;
            notice(state,
                   "verlauf gekuerzt: %zu %s am anfang weggelassen "
                   "(budget ~%zu tokens)",
                   from, (from == 1) ? "nachricht" : "nachrichten", budget);
        }
    }

    req->est = ctx_tokens_request(chat, from, req->system, ctx->summary);
    state->ctx.dropped = from;

    /* summary einmal pro runde einpacken (besitz beim req, teardown
     * gibt frei); build borgt sie nur */
    if (ctx->summary != NULL) {
        req->summary_wrapped = ctx_summary_wrap(ctx->summary);
        if (req->summary_wrapped == NULL) {
            die("out of memory");
        }
    }

    int n = send_build_messages_from(chat, from, req->system,
                                     req->summary_wrapped, &req->msgs);
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
    if (send_setup(state, cfg, &req) != 0) {
        return -1;
    }

    /* der client lebt fuer den GESAMTEN turn (request und eine
     * evtl. compaction davor auf derselben verbindung) */
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

    if (send_round_build(state, cfg, &req, &client, NULL) != 0) {
        oai_client_free(&client);
        return -1;
    }

    OaiChatCompletionParams params = {
        .model = req.model->id,
        .messages = req.msgs,
        .messages_len = (size_t)req.n,
    };

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
    void (*tick)(void *);  /* leichter frame (spinner), fallback redraw */
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
     * rahmenzeile einfrieren wuerden. der leichte tick ueber-
     * schreibt nur die spinner-zeilen: ein voller frame in diesem
     * takt liesse die ganze input-leiste flackern */
    if (sc->tick != NULL) {
        sc->tick(sc->redraw_ctx);
    } else if (sc->redraw != NULL) {
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
    dbg("stream: fehler http=%ld msg=%s", http_status,
        (message != NULL) ? message : "?");

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
    atomic_int refs;  /* besitzer: worker + aufrufer (join) oder nur
                       * worker (abgekoppelt beim timeout) – wer die
                       * letzte referenz abgibt, raeumt auf */
} ToolJob;

/* die letzte referenz gibt alles frei. result ist NULL, sobald
 * der aufrufer es uebernommen hat (join-erfolg). */
static void tool_job_unref(ToolJob *job)
{
    if (atomic_fetch_sub(&job->refs, 1) == 1) {
        free(job->result);
        free(job->name);
        free(job->args);
        free(job);
    }
}

static void *tool_thread(void *ud)
{
    ToolJob *job = ud;
    dbg("tool: thread start (%s)", job->name);
    long long t0 = mono_ms();
    job->result = tool_execute(job->name, job->args);
    job->dur_ms = mono_ms() - t0;
    dbg("tool: thread fertig (%s, %lldms)", job->name, job->dur_ms);
    atomic_store(&job->done, true);
    tool_job_unref(job);
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
    atomic_init(&job->refs, 2); /* worker + aufrufer */

    pthread_t th;
    if (pthread_create(&th, NULL, tool_thread, job) != 0) {
        die("thread create failed"); /* ressourcen alle: ehrlich scheitern */
    }
    dbg("tool: thread erzeugt (%s)", job->name);

    while (!atomic_load(&job->done)) {
        /* ui-tick: spinner (chat + rahmen) und sekundenzaehler
         * leben weiter, solange das tool arbeitet */
        if (hooks->tick != NULL) {
            hooks->tick(hooks->ctx);
        } else {
            hooks->redraw(hooks->ctx);
        }
        if (keys_abort_pressed()) {
            /* bash-kind killen (ganze prozessgruppe inkl. enkel-
             * kinder) – der worker kehrt danach sofort aus dem
             * read zurueck. DANN erst joinen: ein join vorher
             * wuerde auf das haengende kommando warten */
            dbg("tool: abbruch – kill prozessgruppe");
            tool_kill_current();

            /* gnadenfrist: killbare tools (bash) enden sofort.
             * nicht-killbare (read_file auf einem haengenden fs)
             * bekommen zwei sekunden, danach werden sie abgekoppelt
             * – die app blockiert NIE. der worker raeumt sein job
             * selbst ab (heap, strings sind kopiert). */
            struct timespec grace = {2, 0};
            if (pthread_timedjoin_np(th, NULL, &grace) == 0) {
                tool_job_unref(job); /* wir waren als letztes dran */
            } else {
                dbg("tool: worker nach 2s nicht beendet – abgekoppelt");
                (void)pthread_detach(th);
                tool_job_unref(job); /* der worker raeumt, wenn er
                                      * irgendwann endet (refs 2 -> 1) */
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
    tool_job_unref(job);
    return true;
}

/* EINEN ganzen tool-call-stapel parallel ausfuehren: alle worker
 * gleichzeitig starten, gemeinsam auf fertig + abbruch pollen, die
 * ergebnisse IN CALL-REIHENFOLGE einsammeln (wie der pi-agent: das
 * transcript und das session-log bleiben deterministisch). nur
 * nebenlaeufig harmlose tools (read_file, bash) – die runde ruft
 * das gar nicht erst auf, wenn ein datei-mutierendes tool dabei
 * ist (tool_is_sequential).
 *
 * results[i] und durs[i] = ergebnis + dauer des i-ten calls
 * (owned). rueckgabe false = benutzer-abbruch (ergebnisse verworfen,
 * laufende bash-kids gekillt, haengende worker abgekoppelt). */
static bool tool_run_batch(const ChatToolCall *calls, size_t len,
                           const SendHooks *hooks, char **results,
                           long long *durs)
{
    ToolJob *jobs[SEND_MAX_TOOLS];
    pthread_t ths[SEND_MAX_TOOLS];
    if (len > SEND_MAX_TOOLS) {
        len = SEND_MAX_TOOLS;
    }

    for (size_t i = 0; i < len; i++) {
        ToolJob *job = calloc(1, sizeof *job);
        if (job == NULL) {
            die("out of memory");
        }
        job->name = dup_str((calls[i].name != NULL) ? calls[i].name : "?");
        job->args =
            dup_str((calls[i].arguments != NULL) ? calls[i].arguments : "");
        if (job->name == NULL || job->args == NULL) {
            die("out of memory");
        }
        atomic_init(&job->done, false);
        atomic_init(&job->refs, 2); /* worker + aufrufer */
        jobs[i] = job;
        if (pthread_create(&ths[i], NULL, tool_thread, job) != 0) {
            die("thread create failed");
        }
        dbg("tool: thread erzeugt (%s)", job->name);
    }

    while (true) {
        size_t remaining = 0;
        for (size_t i = 0; i < len; i++) {
            if (!atomic_load(&jobs[i]->done)) {
                remaining++;
            }
        }
        if (remaining == 0) {
            break;
        }
        /* ui-tick: spinner und sekundenzaehler leben weiter, solange
         * irgendein tool arbeitet */
        if (hooks->tick != NULL) {
            hooks->tick(hooks->ctx);
        } else {
            hooks->redraw(hooks->ctx);
        }
        if (keys_abort_pressed()) {
            /* ALLE laufenden bash-kinder killen (prozessgruppen),
             * dann die gnadenfrist wie beim einzelnen call: killbare
             * worker enden sofort, haengende werden abgekoppelt */
            dbg("tool: abbruch – kill alle prozessgruppen");
            tool_kill_current();
            for (size_t i = 0; i < len; i++) {
                if (atomic_load(&jobs[i]->done)) {
                    /* fertig, aber noch nicht gejoint: ergebnis
                     * verwerfen und die eigene referenz abgeben */
                    (void)pthread_join(ths[i], NULL);
                    tool_job_unref(jobs[i]);
                    continue;
                }
                struct timespec grace = {2, 0};
                if (pthread_timedjoin_np(ths[i], NULL, &grace) == 0) {
                    tool_job_unref(jobs[i]);
                } else {
                    dbg("tool: worker %zu nach 2s nicht beendet – "
                        "abgekoppelt",
                        i);
                    (void)pthread_detach(ths[i]);
                    tool_job_unref(jobs[i]);
                }
            }
            return false;
        }
        struct timespec ts = {0, 50L * 1000L * 1000L};
        (void)nanosleep(&ts, NULL);
    }

    for (size_t i = 0; i < len; i++) {
        (void)pthread_join(ths[i], NULL);
        durs[i] = jobs[i]->dur_ms;
        results[i] = jobs[i]->result;
        jobs[i]->result = NULL; /* eigentum an den aufrufer */
        tool_job_unref(jobs[i]);
    }
    return true;
}

/* der runden-loop eines turns: runden bauen, requests streamen,
 * tools ausfuehren, bis die antwort ohne tool-calls steht. alle
 * requests laufen auf DEM turn-client (eine verbindung, kein
 * tls-handshake pro runde). der aufrufer (send_stream) kuemmert
 * sich um client + req-ressourcen, die exits hier sind reine
 * ergebnis-codes: 0 = turn fertig (antwort oder abbruch), -1 =
 * fehler (die meldung steht im verlauf). */
static int send_stream_loop(AppState *state, const Config *cfg, SendReq *req,
                            OaiClient *client, const OaiTool *tools,
                            size_t tools_len, const SendHooks *hooks,
                            long long turn_start)
{
    Chat *chat = &state->chat;

    for (int round = 0;; round++) {
        if (send_round_build(state, cfg, req, client, hooks) != 0) {
            send_teardown(req);
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
            .model = req->model->id,
            .messages = req->msgs,
            .messages_len = (size_t)req->n,
            .tools = tools,
            .tools_len = tools_len,
            .include_usage = true, /* letzter chunk: token-zaehlung */
        };

        StreamCtx sc = {
            .chat = chat,
            .session = &state->session,
            .model = (req->model->id != NULL) ? req->model->id : NULL,
            .round = round,
            .t_start = mono_ms(),
            .turn_start = turn_start,
            .redraw_ctx = hooks->ctx,
            .redraw = hooks->redraw,
            .tick = hooks->tick,
        };
        OaiStreamCallbacks cbs = {
            .on_chunk = stream_on_chunk,
            .on_error = stream_on_error,
            .should_abort = stream_should_abort,
            .user_data = &sc,
        };

        int rc = oai_chat_completions_create_stream(client, &params, &cbs);
        send_teardown(req);

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
        ctx_calibrate(&state->ctx, req->est, sc.prompt_tokens);
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

        /* datei-mutierende tools duerfen einander nicht in die quere
         * kommen: ist EIN sequential-tool im stapel, laeuft ALLES
         * sequenziell (wie der pi-agent seine batches handhabt).
         * ansonsten: alle parallel – eine runde mit fuenf reads
         * dauert dann so lange wie eine (statt fuenf). */
        bool has_seq = false;
        for (size_t i = 0; i < calls_len; i++) {
            if (tool_is_sequential(calls[i].name)) {
                has_seq = true;
            }
        }

        /* voller frame vor dem ersten call: die call-zeilen sind
         * committet (der letzte frame war noch streaming), der
         * spinner laeuft sichtbar darunter */
        state->tool_running = true;
        hooks->redraw(hooks->ctx);

        bool ok = true;
        if (has_seq) {
            /* sequenziell: wie bisher, ein call nach dem anderen */
            for (size_t i = 0; i < calls_len && ok; i++) {
                ChatToolCall *call = &calls[i];
                long long dur = -1;
                char *result = NULL;
                dbg("tool-loop: call %zu/%zu (%s)", i + 1, calls_len,
                    (call->name != NULL) ? call->name : "?");
                ok = tool_run_async(call, hooks, &result, &dur);
                if (ok) {
                    if (result == NULL) {
                        die("out of memory");
                    }
                    (void)session_log_tool(&state->session, call->id,
                                           call->name, result, dur);
                    if (chat_append_tool(chat, call->id, result) != 0) {
                        die("out of memory");
                    }
                    free(result);
                    /* voller frame: das ergebnis drucken, der
                     * chat-spinner weicht dem output */
                    hooks->redraw(hooks->ctx);
                }
            }
        } else {
            /* parallel: alles gleichzeitig, ergebnisse in call-
             * reihenfolge einsammeln (transcript + log bleiben
             * deterministisch) */
            char *results[SEND_MAX_TOOLS];
            long long durs[SEND_MAX_TOOLS];
            dbg("tool-loop: %zu calls parallel", calls_len);
            ok = tool_run_batch(calls, calls_len, hooks, results, durs);
            if (ok) {
                for (size_t i = 0; i < calls_len; i++) {
                    ChatToolCall *call = &calls[i];
                    if (results[i] == NULL) {
                        die("out of memory");
                    }
                    (void)session_log_tool(&state->session, call->id,
                                           call->name, results[i], durs[i]);
                    if (chat_append_tool(chat, call->id, results[i]) != 0) {
                        die("out of memory");
                    }
                    free(results[i]);
                    hooks->redraw(hooks->ctx);
                }
            }
        }
        state->tool_running = false;
        if (!ok) {
            /* abbruch: angefangene calls bleiben ohne antwort
             * (wie beim stream-abbruch), die runde endet hier */
            notice(state, "abgebrochen");
            return 0;
        }
    }
}

int send_stream(AppState *state, const Config *cfg, const SendHooks *hooks)
{
    if (state == NULL || cfg == NULL || hooks == NULL ||
        hooks->redraw == NULL) {
        return -1;
    }

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
    dbg("turn: beginn (busy_start=%lld)", turn_start);

    /* der client gehoert dem GESAMTEN turn: alle runden (und eine
     * evtl. compaction davor) laufen auf derselben verbindung –
     * curl behaelt den verbindungs-cache im easy-handle, keep-alive
     * und die tls-session bleiben ueber die runden erhalten. pro
     * runde einen neuen client zu bauen hiesse: pro runde ein
     * handshake (openai_completions.c kommentiert genau das an).
     * der loop läuft sequenziell auf dem haupt-thread, auch die
     * tool-threads fassen den client nie an. */
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

    int rc = send_stream_loop(state, cfg, &req, &client, tools, tools_len,
                              hooks, turn_start);

    oai_client_free(&client);
    send_teardown(&req); /* no-op nach gelaufenen runden, aufraeumen
                          * nach einem abbruch in round_build */
    return rc;
}
