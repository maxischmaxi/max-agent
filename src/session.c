#define _POSIX_C_SOURCE 200809L // NOLINT(bugprone-reserved-identifier)

#include "session.h"

#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "cJSON.h"
#include "debug.h"
#include "utils.h"

/* ------------------------------------------------------------------ */
/* pfade                                                               */
/* ------------------------------------------------------------------ */

/* ~/.config/.maxagent/sessions – wie die config am selben ort. */
static char *sessions_dir(void)
{
    return append_to_home(".config/.maxagent/sessions");
}

/* aktuelles arbeitsverzeichnis als heap-string. NULL = getcwd
 * scheiterte (geloeschtes verzeichnis, keine rechte): die session
 * laeuft dann ohne ordner-bindung und taucht in keinem resume-
 * dialog auf. der puffer ist PATH_MAX, nicht dynamisch – laengere
 * pfade gibt es praktisch nicht, und realloc-schleifen um ein
 * getcwd herum lohnen den aufwand nicht. */
static char *cwd_current(void)
{
    char buf[PATH_MAX];
    if (getcwd(buf, sizeof buf) == NULL) {
        return NULL;
    }
    return dup_str(buf);
}

static char *meta_path(const char *id)
{
    char *dir = sessions_dir();
    if (dir == NULL) {
        return NULL;
    }
    size_t n = strlen(dir) + 1 + strlen(id) + strlen(".json") + 1;
    char *path = malloc(n);
    if (path == NULL) {
        free(dir);
        return NULL;
    }
    (void)snprintf(path, n, "%s/%s.json", dir, id);
    free(dir);
    return path;
}

static char *log_path(const char *id)
{
    char *dir = sessions_dir();
    if (dir == NULL) {
        return NULL;
    }
    size_t n = strlen(dir) + 1 + strlen(id) + strlen(".jsonl") + 1;
    char *path = malloc(n);
    if (path == NULL) {
        free(dir);
        return NULL;
    }
    (void)snprintf(path, n, "%s/%s.jsonl", dir, id);
    free(dir);
    return path;
}

int session_dir_ensure(void)
{
    char *dir = sessions_dir();
    if (dir == NULL) {
        return -1;
    }
    int rc = mkdir_p(dir, 0755);
    free(dir);
    return rc;
}

/* ------------------------------------------------------------------ */
/* zeit und id                                                         */
/* ------------------------------------------------------------------ */

static long long now_ms(void)
{
    struct timespec ts;
    (void)clock_gettime(CLOCK_REALTIME, &ts);
    return ((long long)ts.tv_sec * 1000) + (ts.tv_nsec / 1000000);
}

/* "s-<hex(unix-ms)>-<6 hex zufall>": zeitpraefix macht die liste
 * chronologisch sortierbar, zufall kollisionsfrei. /dev/urandom
 * ist praktisch immer da; der fallback mischt monotone uhr und
 * pid – reicht, weil die id ohnehin nur pro rechner eindeutig
 * sein muss. */
static void id_generate(char *out, size_t sz)
{
    unsigned char rnd[3];
    bool have = false;
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd >= 0) {
        have = (read(fd, rnd, sizeof rnd) == (ssize_t)sizeof rnd);
        (void)close(fd);
    }
    if (!have) {
        struct timespec ts;
        (void)clock_gettime(CLOCK_MONOTONIC, &ts);
        unsigned x = (unsigned)(ts.tv_nsec ^ ((unsigned)getpid() << 12));
        for (size_t i = 0; i < sizeof rnd; i++) {
            rnd[i] = (unsigned char)(x >> (i * 8));
        }
    }
    (void)snprintf(out, sz, "s-%llx-%02x%02x%02x", (unsigned long long)now_ms(),
                   rnd[0], rnd[1], rnd[2]);
}

/* alle heap-felder freigeben, struct zero setzen */
static void session_reset_fields(Session *s)
{
    free(s->name);
    s->name = NULL;
    free(s->model);
    s->model = NULL;
    free(s->base_url);
    s->base_url = NULL;
    free(s->system_prompt);
    s->system_prompt = NULL;
    free(s->cwd);
    s->cwd = NULL;
    s->id[0] = '\0';
    s->created_at = 0;
    s->updated_at = 0;
    s->worked_ms = 0;
    s->messages = 0;
}

/* endet das log nicht auf '\n' (crash mitten im fwrite), wird die
 * abgerissene zeile mit einem newline abgeschlossen: sie bleibt
 * als unparsebarer muell stehen, aber der naechste append haengt
 * sauber an, statt mit ihr zu einer zeile zu verschmelzen. */
static void heal_torn_log(FILE *log)
{
    if (log == NULL) {
        return;
    }
    if (fseek(log, 0, SEEK_END) != 0) {
        return;
    }
    long size = ftell(log);
    if (size <= 0) {
        return;
    }
    if (fseek(log, -1, SEEK_END) != 0) {
        return;
    }
    int c = fgetc(log);
    clearerr(log);
    if (c != EOF && c != '\n') {
        (void)fputc('\n', log); /* append-modus: landet am ende */
        (void)fflush(log);
    }
    (void)fseek(log, 0, SEEK_END);
}

/* ------------------------------------------------------------------ */
/* meta-file                                                           */
/* ------------------------------------------------------------------ */

static int meta_write(const Session *s)
{
    if (!s->active) {
        return -1;
    }

    cJSON *o = cJSON_CreateObject();
    if (o == NULL) {
        return -1;
    }
    cJSON_AddStringToObject(o, "id", s->id);
    if (s->name != NULL) {
        cJSON_AddStringToObject(o, "name", s->name);
    } else {
        cJSON_AddNullToObject(o, "name");
    }
    cJSON_AddNumberToObject(o, "created_at", (double)s->created_at);
    cJSON_AddNumberToObject(o, "updated_at", (double)s->updated_at);
    cJSON_AddNumberToObject(o, "worked_ms", (double)s->worked_ms);
    cJSON_AddNumberToObject(o, "messages", (double)s->messages);
    if (s->model != NULL) {
        cJSON_AddStringToObject(o, "model", s->model);
    } else {
        cJSON_AddNullToObject(o, "model");
    }
    if (s->base_url != NULL) {
        cJSON_AddStringToObject(o, "base_url", s->base_url);
    } else {
        cJSON_AddNullToObject(o, "base_url");
    }
    if (s->system_prompt != NULL) {
        cJSON_AddStringToObject(o, "system_prompt", s->system_prompt);
    } else {
        cJSON_AddNullToObject(o, "system_prompt");
    }
    if (s->cwd != NULL) {
        cJSON_AddStringToObject(o, "cwd", s->cwd);
    } else {
        cJSON_AddNullToObject(o, "cwd");
    }
    cJSON_AddStringToObject(o, "version", SESSION_VERSION);

    char *json = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    if (json == NULL) {
        return -1;
    }

    char *path = meta_path(s->id);
    int rc = -1;
    if (path != NULL) {
        rc = write_file(path, json, strlen(json));
        free(path);
    }
    cJSON_free(json);
    return rc;
}

/* ------------------------------------------------------------------ */
/* migration                                                           */
/* ------------------------------------------------------------------ */

/* version-1-metas kennen kein "cwd"-feld: sie stammen aus der zeit,
 * als sessionen global sichtbar waren. beim app-start einmal dr-
 * ueberlaufen und jedem das aktuelle arbeitsverzeichnis ein-
 * schreiben (version 2). idempotent: ein meta mit "cwd" bleibt
 * unberuehrt, ein unlesbares meta blockiert die restlichen nicht. */
int sessions_migrate_legacy(void)
{
    if (session_dir_ensure() != 0) {
        return -1;
    }
    char *dir_path = sessions_dir();
    if (dir_path == NULL) {
        return -1;
    }
    DIR *d = opendir(dir_path);
    if (d == NULL) {
        free(dir_path);
        return -1;
    }

    char *cwd = cwd_current();
    struct dirent *e;
    int rc = 0;
    while ((e = readdir(d)) != NULL) {
        size_t len = strlen(e->d_name);
        /* nur meta-dateien: ".json" am ende – ".jsonl" endet auf
         * 'l' und faellt dadurch raus */
        if (len < strlen(".json") + 1 ||
            strcmp(e->d_name + len - strlen(".json"), ".json") != 0) {
            continue;
        }
        char *path = malloc(strlen(dir_path) + 1 + len + 1);
        if (path == NULL) {
            rc = -1;
            break;
        }
        (void)snprintf(path, strlen(dir_path) + 1 + len + 1, "%s/%s", dir_path,
                       e->d_name);
        cJSON *meta = parse_json_file(path);
        if (meta == NULL) {
            free(path);
            continue; /* kaputtes meta: migration der anderen weiter */
        }
        /* fehlt "cwd" oder ist es null/leer, bekommt das meta das
         * aktuelle arbeitsverzeichnis und version 2. ein meta mit
         * echtem cwd bleibt komplett unberuehrt. */
        const cJSON *jcwd = cJSON_GetObjectItemCaseSensitive(meta, "cwd");
        bool needs_cwd = !cJSON_IsString(jcwd) || jcwd->valuestring[0] == '\0';
        if (needs_cwd && cwd != NULL) {
            cJSON *item = cJSON_CreateString(cwd);
            if (item == NULL) {
                rc = -1; /* OOM: abbrechen, nichts verloren */
                cJSON_Delete(meta);
                free(path);
                break;
            }
            if (!cJSON_ReplaceItemInObjectCaseSensitive(meta, "cwd", item)) {
                /* feld fehlte: neu anlegen (AddItem uebernimmt item) */
                cJSON_AddItemToObject(meta, "cwd", item);
            }
            cJSON_ReplaceItemInObjectCaseSensitive(
                meta, "version", cJSON_CreateString(SESSION_VERSION));
            char *json = cJSON_PrintUnformatted(meta);
            if (json == NULL) {
                rc = -1;
            } else if (write_file(path, json, strlen(json)) != 0) {
                rc = -1;
            }
            cJSON_free(json);
        }
        cJSON_Delete(meta);
        free(path);
        if (rc != 0) {
            break;
        }
    }
    (void)closedir(d);
    free(dir_path);
    free(cwd);
    return rc;
}

/* ------------------------------------------------------------------ */
/* lebenzyklus                                                         */
/* ------------------------------------------------------------------ */

/* base_url zum aktiven modell suchen – bewusst ohne send.h, damit
 * session.c nichts von der ui/import-kette mitzieht. */
static char *provider_url_for(const Config *cfg, const char *model_id)
{
    if (cfg == NULL || model_id == NULL) {
        return NULL;
    }
    for (size_t p = 0; p < cfg->providers_len; p++) {
        const Provider *pr = &cfg->providers[p];
        for (size_t m = 0; m < pr->models_len; m++) {
            if (pr->models[m].id != NULL &&
                strcmp(pr->models[m].id, model_id) == 0 &&
                pr->base_url != NULL) {
                return dup_str(pr->base_url);
            }
        }
    }
    return NULL;
}

/* dateien einer session loeschen (nur fuer den fehlgeschlagenen
 * session_start: halbfertige sessions sollen kein muell im
 * verzeichnis hinterlassen, das sonst niemand mehr los wird) */
static void session_remove_files(const char *id)
{
    char *mp = meta_path(id);
    if (mp != NULL) {
        (void)remove(mp);
        free(mp);
    }
    char *lp = log_path(id);
    if (lp != NULL) {
        (void)remove(lp);
        free(lp);
    }
}

int session_start(Session *s, const Config *cfg)
{
    session_end(s); /* defensive: nichts offen lassen */
    if (session_dir_ensure() != 0) {
        return -1;
    }

    /* freie id suchen: wuerfeln, bis keine meta-datei existiert */
    char id[SESSION_ID_MAX];
    bool found = false;
    for (int tries = 0; tries < 16; tries++) {
        id_generate(id, sizeof id);
        char *mp = meta_path(id);
        if (mp == NULL) {
            return -1;
        }
        bool exists = is_file(mp);
        free(mp);
        if (exists) {
            continue; /* vergeben: nochmal wuerfeln */
        }
        found = true;
        break;
    }
    if (!found) {
        return -1;
    }

    (void)snprintf(s->id, sizeof s->id, "%s", id);
    s->created_at = now_ms();
    s->updated_at = s->created_at;

    if (cfg != NULL) {
        if (cfg->active_model != NULL) {
            s->model = dup_str(cfg->active_model);
        }
        s->base_url = provider_url_for(cfg, cfg->active_model);
        if (cfg->system_prompt != NULL) {
            s->system_prompt = dup_str(cfg->system_prompt);
        }
    }
    s->cwd = cwd_current();

    char *lp = log_path(s->id);
    if (lp == NULL) {
        session_reset_fields(s);
        return -1;
    }
    /* "a+": schreibt wie "a" immer ans ende, erlaubt aber lese-
     * zugriffe (s. session_open/heal_torn_log) */
    s->log = fopen(lp, "a+");
    free(lp);
    if (s->log == NULL) {
        session_reset_fields(s);
        return -1;
    }

    s->active = true;
    if (meta_write(s) != 0) {
        /* meta unbeschreibbar (fs voll/ro?): log ohne meta waere
         * eine session, die niemand mehr oeffnen kann – dateien
         * wieder wegwerfen */
        (void)fflush(s->log);
        (void)fclose(s->log);
        s->log = NULL;
        s->active = false;
        session_remove_files(s->id);
        session_reset_fields(s);
        return -1;
    }
    dbg("session: start %s", s->id);
    dbg_rename(s->id);
    return 0;
}

/* meta-einlesen; s wird dabei als bereits leer erwartet. -1 = weg
 * damit oder kaputt. */
static int meta_read(Session *s, cJSON *meta)
{
    const cJSON *jid = cJSON_GetObjectItemCaseSensitive(meta, "id");
    if (!cJSON_IsString(jid) || strlen(jid->valuestring) >= sizeof s->id) {
        return -1;
    }
    (void)snprintf(s->id, sizeof s->id, "%s", jid->valuestring);

    const cJSON *jname = cJSON_GetObjectItemCaseSensitive(meta, "name");
    if (cJSON_IsString(jname)) {
        s->name = dup_str(jname->valuestring);
    }
    const cJSON *jmodel = cJSON_GetObjectItemCaseSensitive(meta, "model");
    if (cJSON_IsString(jmodel)) {
        s->model = dup_str(jmodel->valuestring);
    }
    const cJSON *jurl = cJSON_GetObjectItemCaseSensitive(meta, "base_url");
    if (cJSON_IsString(jurl)) {
        s->base_url = dup_str(jurl->valuestring);
    }
    const cJSON *jprompt =
        cJSON_GetObjectItemCaseSensitive(meta, "system_prompt");
    if (cJSON_IsString(jprompt)) {
        s->system_prompt = dup_str(jprompt->valuestring);
    }
    const cJSON *jcwd = cJSON_GetObjectItemCaseSensitive(meta, "cwd");
    if (cJSON_IsString(jcwd)) {
        s->cwd = dup_str(jcwd->valuestring);
    }
    const cJSON *jcreated =
        cJSON_GetObjectItemCaseSensitive(meta, "created_at");
    s->created_at =
        cJSON_IsNumber(jcreated) ? (long long)jcreated->valuedouble : now_ms();
    const cJSON *jupdated =
        cJSON_GetObjectItemCaseSensitive(meta, "updated_at");
    s->updated_at = cJSON_IsNumber(jupdated) ? (long long)jupdated->valuedouble
                                             : s->created_at;
    const cJSON *jmsgs = cJSON_GetObjectItemCaseSensitive(meta, "messages");
    s->messages = cJSON_IsNumber(jmsgs) ? (size_t)jmsgs->valuedouble : 0;
    const cJSON *jworked = cJSON_GetObjectItemCaseSensitive(meta, "worked_ms");
    s->worked_ms =
        cJSON_IsNumber(jworked) ? (long long)jworked->valuedouble : 0;

    /* name == NULL darf nicht heissen "allokation gescheitert" –
     * dup_str OOM waere fatal, das meta bleibt trotzdem brauchbar */
    return 0;
}

/* session-ids haben ein festes format ("s-<hex>-<hex>"). alles
 * andere – auch pfade wie "../../x" – kommt nicht aus dieser app
 * und wird abgewiesen: die id landet in dateipfaden. */
static bool valid_id(const char *id)
{
    static const char id_chars[] = "0123456789abcdef-";
    if (id == NULL || id[0] != 's' || id[1] != '-') {
        return false;
    }
    for (const char *p = id + 2; *p != '\0'; p++) {
        if (strchr(id_chars, *p) == NULL) {
            return false;
        }
    }
    return true;
}

int session_open(Session *s, const char *id)
{
    dbg("session: open %s", (id != NULL) ? id : "(null)");
    if (id == NULL || id[0] == '\0' || strlen(id) >= sizeof s->id) {
        return -1;
    }
    if (!valid_id(id)) {
        return -1;
    }
    if (s->active && strcmp(id, s->id) == 0) {
        return 0; /* dieselbe session ist schon offen */
    }
    session_end(s);

    char *mp = meta_path(id);
    cJSON *meta = (mp != NULL) ? parse_json_file(mp) : NULL;
    free(mp);
    if (meta == NULL) {
        return -1;
    }
    int rc = meta_read(s, meta);
    cJSON_Delete(meta);
    if (rc != 0) {
        session_reset_fields(s);
        return -1;
    }

    char *lp = log_path(s->id);
    if (lp == NULL) {
        session_reset_fields(s);
        return -1;
    }
    s->log = fopen(lp, "a+");
    free(lp);
    if (s->log == NULL) {
        session_reset_fields(s);
        return -1;
    }
    heal_torn_log(s->log);
    s->active = true;
    return 0;
}

void session_end(Session *s)
{
    if (s->active) {
        dbg("session: end %s (%zu ereignisse)", s->id, s->messages);
    }
    if (s->log != NULL) {
        (void)fflush(s->log);
        (void)fclose(s->log);
        s->log = NULL;
    }
    if (s->active) {
        s->updated_at = now_ms();
        (void)meta_write(s); /* bester versuch, meta ist eh aktuell */
        s->active = false;
    }
    session_reset_fields(s);
}

void session_free(Session *s)
{
    session_end(s);
}

int session_rename(Session *s, const char *name)
{
    if (!s->active || name == NULL || name[0] == '\0') {
        return -1;
    }
    free(s->name);
    s->name = dup_str(name);
    if (s->name == NULL) {
        return -1;
    }
    dbg("session: rename -> %s", name);
    return meta_write(s);
}

int session_prompt_changed(Session *s, const char *prompt)
{
    if (!s->active) {
        return 0; /* keine session offen: nichts zu spiegeln */
    }
    char *copy = NULL;
    if (prompt != NULL) {
        copy = dup_str(prompt);
        if (copy == NULL) {
            return -1;
        }
    }
    free(s->system_prompt);
    s->system_prompt = copy;
    return meta_write(s);
}

/* ------------------------------------------------------------------ */
/* ereignis-log                                                        */
/* ------------------------------------------------------------------ */

static cJSON *event_new(const char *type)
{
    cJSON *o = cJSON_CreateObject();
    if (o != NULL) {
        cJSON_AddStringToObject(o, "type", type);
        cJSON_AddNumberToObject(o, "ts", (double)now_ms());
    }
    return o;
}

/* eine fertige zeile anhaengen und das meta (updated_at/messages)
 * nachziehen. alles hier ist best-effort: schreibt die platte
 * streikt, verlieren wir diese zeile – nicht die app. */
static int log_line(Session *s, cJSON *o)
{
    if (o == NULL) {
        return -1;
    }
    char *line = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    if (line == NULL) {
        return -1;
    }

    int rc = -1;
    if (s->log != NULL) {
        size_t n = strlen(line);
        bool ok = (fwrite(line, 1, n, s->log) == n);
        if (ok) {
            ok = (fputc('\n', s->log) != EOF);
        }
        (void)fflush(s->log);
        if (ok) {
            rc = 0;
            s->messages++;
            s->updated_at = now_ms();
            (void)meta_write(s);
        }
    }
    cJSON_free(line);
    return rc;
}

int session_log_user(Session *s, const char *text)
{
    if (!s->active || text == NULL) {
        return 0;
    }
    cJSON *o = event_new("msg");
    if (o == NULL) {
        return -1;
    }
    cJSON_AddStringToObject(o, "role", "user");
    cJSON_AddStringToObject(o, "text", text);
    return log_line(s, o);
}

void session_worked_set(Session *s, long long worked_ms)
{
    if (s == NULL || !s->active) {
        return;
    }
    s->worked_ms = worked_ms;
    (void)meta_write(s);
}

int session_log_assistant(Session *s, const char *text,
                          const ChatToolCall *calls, size_t calls_len,
                          long long ttft_ms, long long total_ms,
                          long long work_ms, int round, const char *model,
                          int prompt_tokens, int completion_tokens,
                          bool aborted)
{
    if (!s->active) {
        return 0;
    }
    cJSON *o = event_new("msg");
    if (o == NULL) {
        return -1;
    }
    cJSON_AddStringToObject(o, "role", "assistant");
    cJSON_AddStringToObject(o, "text", (text != NULL) ? text : "");
    if (ttft_ms >= 0) {
        cJSON_AddNumberToObject(o, "ttft_ms", (double)ttft_ms);
    }
    if (total_ms >= 0) {
        cJSON_AddNumberToObject(o, "total_ms", (double)total_ms);
    }
    if (work_ms >= 0) {
        /* zeit des GESAMTEN turns: thinking + alle runden + tools */
        cJSON_AddNumberToObject(o, "work_ms", (double)work_ms);
    }
    if (round >= 0) {
        cJSON_AddNumberToObject(o, "round", round);
    }
    if (model != NULL) {
        cJSON_AddStringToObject(o, "model", model);
    }
    if (prompt_tokens >= 0) {
        cJSON_AddNumberToObject(o, "prompt_tokens", prompt_tokens);
    }
    if (completion_tokens >= 0) {
        cJSON_AddNumberToObject(o, "completion_tokens", completion_tokens);
    }
    if (aborted) {
        cJSON_AddBoolToObject(o, "aborted", true);
    }
    if (calls != NULL && calls_len > 0) {
        cJSON *arr = cJSON_AddArrayToObject(o, "tool_calls");
        if (arr != NULL) {
            for (size_t i = 0; i < calls_len; i++) {
                cJSON *c = cJSON_CreateObject();
                if (c == NULL) {
                    continue;
                }
                if (calls[i].id != NULL) {
                    cJSON_AddStringToObject(c, "id", calls[i].id);
                }
                if (calls[i].name != NULL) {
                    cJSON_AddStringToObject(c, "name", calls[i].name);
                }
                if (calls[i].arguments != NULL) {
                    cJSON_AddStringToObject(c, "arguments", calls[i].arguments);
                }
                cJSON_AddItemToArray(arr, c);
            }
        }
    }
    return log_line(s, o);
}

int session_log_tool(Session *s, const char *call_id, const char *name,
                     const char *result, long long dur_ms)
{
    if (!s->active || call_id == NULL) {
        return 0;
    }
    cJSON *o = event_new("tool");
    if (o == NULL) {
        return -1;
    }
    cJSON_AddStringToObject(o, "call_id", call_id);
    if (name != NULL) {
        cJSON_AddStringToObject(o, "name", name);
    }
    cJSON_AddStringToObject(o, "text", (result != NULL) ? result : "");
    if (dur_ms >= 0) {
        cJSON_AddNumberToObject(o, "dur_ms", (double)dur_ms);
    }
    return log_line(s, o);
}

int session_log_error(Session *s, const char *text, long http_status)
{
    if (!s->active || text == NULL) {
        return 0;
    }
    cJSON *o = event_new("error");
    if (o == NULL) {
        return -1;
    }
    cJSON_AddStringToObject(o, "text", text);
    if (http_status > 0) {
        cJSON_AddNumberToObject(o, "http", (double)http_status);
    }
    return log_line(s, o);
}

int session_log_notice(Session *s, const char *text)
{
    if (!s->active || text == NULL) {
        return 0;
    }
    cJSON *o = event_new("notice");
    if (o == NULL) {
        return -1;
    }
    cJSON_AddStringToObject(o, "text", text);
    return log_line(s, o);
}

int session_log_compaction(Session *s, const char *summary, size_t covered)
{
    if (!s->active || summary == NULL || summary[0] == '\0') {
        return 0;
    }
    cJSON *o = event_new("compaction");
    if (o == NULL) {
        return -1;
    }
    cJSON_AddStringToObject(o, "summary", summary);
    cJSON_AddNumberToObject(o, "covered", (double)covered);
    return log_line(s, o);
}

/* ------------------------------------------------------------------ */
/* transcript zurueckspielen                                           */
/* ------------------------------------------------------------------ */

/* chattoolcall-array aus dem json einer msg bauen; NULL bei keiner
 * oder kaputter call-liste. der array gehoert dann dem aufrufer
 * (chat_set_tool_calls uebernimmt ihn). */
static ChatToolCall *calls_from_json(const cJSON *o, size_t *out_len)
{
    *out_len = 0;
    const cJSON *arr = cJSON_GetObjectItemCaseSensitive(o, "tool_calls");
    if (!cJSON_IsArray(arr)) {
        return NULL;
    }
    int n_raw = cJSON_GetArraySize(arr);
    if (n_raw <= 0) {
        return NULL;
    }
    size_t n = (size_t)n_raw;
    ChatToolCall *calls = calloc(n, sizeof *calls);
    if (calls == NULL) {
        return NULL;
    }
    size_t keep = 0;
    const cJSON *item = NULL;
    cJSON_ArrayForEach(item, arr)
    {
        if (!cJSON_IsObject(item) || keep >= n) {
            continue;
        }
        const cJSON *jid = cJSON_GetObjectItemCaseSensitive(item, "id");
        const cJSON *jname = cJSON_GetObjectItemCaseSensitive(item, "name");
        const cJSON *jargs =
            cJSON_GetObjectItemCaseSensitive(item, "arguments");
        if (cJSON_IsString(jname)) {
            calls[keep].id =
                cJSON_IsString(jid) ? dup_str(jid->valuestring) : dup_str("");
            calls[keep].name = dup_str(jname->valuestring);
            calls[keep].arguments = cJSON_IsString(jargs)
                                        ? dup_str(jargs->valuestring)
                                        : dup_str("");
            if (calls[keep].id == NULL || calls[keep].name == NULL ||
                calls[keep].arguments == NULL) {
                free(calls[keep].id);
                free(calls[keep].name);
                free(calls[keep].arguments);
                continue; /* OOM: diesen call verwerfen */
            }
            keep++;
        }
    }
    *out_len = keep;
    if (keep == 0) {
        free(calls);
        return NULL;
    }
    return calls;
}

static int replay_line(const cJSON *o, Chat *chat, CtxUsage *ctx)
{
    const cJSON *jtype = cJSON_GetObjectItemCaseSensitive(o, "type");
    const cJSON *jtext = cJSON_GetObjectItemCaseSensitive(o, "text");
    const char *text = cJSON_IsString(jtext) ? jtext->valuestring : "";

    if (cJSON_IsString(jtype) && strcmp(jtype->valuestring, "msg") == 0) {
        const cJSON *jrole = cJSON_GetObjectItemCaseSensitive(o, "role");
        if (!cJSON_IsString(jrole)) {
            return 0;
        }
        if (strcmp(jrole->valuestring, "user") == 0) {
            return chat_append(chat, CHAT_ROLE_USER, text);
        }
        if (strcmp(jrole->valuestring, "assistant") == 0) {
            if (chat_append(chat, CHAT_ROLE_ASSISTANT, text) != 0) {
                return -1;
            }
            size_t calls_len = 0;
            ChatToolCall *calls = calls_from_json(o, &calls_len);
            if (calls != NULL &&
                chat_set_tool_calls(chat, calls, calls_len) != 0) {
                for (size_t i = 0; i < calls_len; i++) {
                    free(calls[i].id);
                    free(calls[i].name);
                    free(calls[i].arguments);
                }
                free(calls);
                return -1;
            }
            /* ctx-gesamtzaehler rekonstruieren: nur echte zaehlungen */
            const cJSON *jp =
                cJSON_GetObjectItemCaseSensitive(o, "prompt_tokens");
            const cJSON *jc =
                cJSON_GetObjectItemCaseSensitive(o, "completion_tokens");
            if (ctx != NULL && cJSON_IsNumber(jp) && cJSON_IsNumber(jc)) {
                ctx_account(ctx, (int)jp->valuedouble, (int)jc->valuedouble);
            }
            return 0;
        }
        return 0;
    }
    if (cJSON_IsString(jtype) && strcmp(jtype->valuestring, "tool") == 0) {
        const cJSON *jcall = cJSON_GetObjectItemCaseSensitive(o, "call_id");
        if (!cJSON_IsString(jcall)) {
            return 0;
        }
        return chat_append_tool(chat, jcall->valuestring, text);
    }
    if (cJSON_IsString(jtype) && strcmp(jtype->valuestring, "error") == 0) {
        return chat_append(chat, CHAT_ROLE_ERROR, text);
    }
    if (cJSON_IsString(jtype) && strcmp(jtype->valuestring, "notice") == 0) {
        return chat_append(chat, CHAT_ROLE_NOTICE, text);
    }
    if (cJSON_IsString(jtype) &&
        strcmp(jtype->valuestring, "compaction") == 0) {
        /* die letzte compaction gewinnt: summary + watermark sind
         * zustand des CtxUsage, nicht des chats. ctx == NULL =
         * niemand interessiert sich dafuer */
        const cJSON *jsum = cJSON_GetObjectItemCaseSensitive(o, "summary");
        const cJSON *jcov = cJSON_GetObjectItemCaseSensitive(o, "covered");
        if (ctx == NULL || !cJSON_IsString(jsum) ||
            jsum->valuestring[0] == '\0') {
            return 0;
        }
        char *copy = dup_str(jsum->valuestring);
        if (copy == NULL) {
            return -1;
        }
        free(ctx->summary);
        ctx->summary = copy;
        ctx->covered = cJSON_IsNumber(jcov) ? (size_t)jcov->valuedouble : 0;
        ctx->compact_failed = false;
        return 0;
    }
    return 0; /* unbekannter typ: ignorieren (vorwaerts-kompatibel) */
}

int session_read_transcript(const Session *s, Chat *chat, CtxUsage *ctx)
{
    if (s->id[0] == '\0' || chat == NULL) {
        return -1;
    }
    char *lp = log_path(s->id);
    if (lp == NULL) {
        return -1;
    }
    FILE *f = fopen(lp, "r");
    free(lp);
    if (f == NULL) {
        return 0; /* meta ohne transcript: leerer chat ist ok */
    }

    char *line = NULL;
    size_t cap = 0;
    ssize_t n;
    int rc = 0;
    while ((n = getline(&line, &cap, f)) != -1) {
        /* abgerissene letzte zeile (crash mitten im fwrite): kein
         * '\n' am ende -> das restliche file ist unbrauchbar, hier
         * ist eh schluss */
        if (n == 0 || line[n - 1] != '\n') {
            break;
        }
        line[n - 1] = '\0';
        cJSON *o = cJSON_Parse(line);
        if (o == NULL) {
            continue; /* kaputte einzelzeile: ueberspringen */
        }
        if (replay_line(o, chat, ctx) != 0) {
            rc = -1;
        }
        cJSON_Delete(o);
    }
    free(line);
    (void)fclose(f);
    return rc;
}

/* ------------------------------------------------------------------ */
/* session-liste                                                       */
/* ------------------------------------------------------------------ */

/* preview: erste user-nachricht, auf 60 byte gekappt (mitten in
 * utf-8 geschnittene sequenzen werden abgeschnitten, bis der
 * schnitt an einer zeichengrenze liegt). */
static void load_preview(const char *id, char **out)
{
    *out = NULL;
    char *lp = log_path(id);
    if (lp == NULL) {
        return;
    }
    FILE *f = fopen(lp, "r");
    free(lp);
    if (f == NULL) {
        return;
    }

    char *line = NULL;
    size_t cap = 0;
    ssize_t n;
    while (*out == NULL && (n = getline(&line, &cap, f)) != -1) {
        if (n == 0 || line[n - 1] != '\n') {
            break;
        }
        line[n - 1] = '\0';
        cJSON *o = cJSON_Parse(line);
        if (o == NULL) {
            continue;
        }
        const cJSON *jtype = cJSON_GetObjectItemCaseSensitive(o, "type");
        const cJSON *jrole = cJSON_GetObjectItemCaseSensitive(o, "role");
        const cJSON *jtext = cJSON_GetObjectItemCaseSensitive(o, "text");
        if (cJSON_IsString(jtype) && strcmp(jtype->valuestring, "msg") == 0 &&
            cJSON_IsString(jrole) && strcmp(jrole->valuestring, "user") == 0 &&
            cJSON_IsString(jtext) && jtext->valuestring[0] != '\0') {
            size_t cut = strlen(jtext->valuestring);
            if (cut > 60) {
                cut = 60;
                while (cut > 0 && ((unsigned char)jtext->valuestring[cut] &
                                   0xC0U) == 0x80U) {
                    cut--; /* nicht mitten im zeichen */
                }
                char *buf = malloc(cut + 1);
                if (buf != NULL) {
                    memcpy(buf, jtext->valuestring, cut);
                    buf[cut] = '\0';
                    *out = buf;
                }
            } else {
                *out = dup_str(jtext->valuestring);
            }
        }
        cJSON_Delete(o);
    }
    free(line);
    (void)fclose(f);
}

/* meta -> SessionInfo. 0 = ok (info gefuellt oder ignoriert). */
static int list_add_meta(SessionList *l, cJSON *meta)
{
    const cJSON *jid = cJSON_GetObjectItemCaseSensitive(meta, "id");
    if (!cJSON_IsString(jid) || strlen(jid->valuestring) >= SESSION_ID_MAX) {
        return 0; /* kaputt, aber kein grund die liste abzubrechen */
    }
    SessionInfo si = {0};
    (void)snprintf(si.id, sizeof si.id, "%s", jid->valuestring);
    const cJSON *jname = cJSON_GetObjectItemCaseSensitive(meta, "name");
    if (cJSON_IsString(jname)) {
        si.name = dup_str(jname->valuestring);
    }
    const cJSON *jupdated =
        cJSON_GetObjectItemCaseSensitive(meta, "updated_at");
    si.updated_at =
        cJSON_IsNumber(jupdated) ? (long long)jupdated->valuedouble : 0;
    const cJSON *jmsgs = cJSON_GetObjectItemCaseSensitive(meta, "messages");
    si.messages = cJSON_IsNumber(jmsgs) ? (size_t)jmsgs->valuedouble : 0;
    load_preview(si.id, &si.preview);

    SessionInfo *grown = realloc(l->items, (l->len + 1) * sizeof *grown);
    if (grown == NULL) {
        free(si.name);
        free(si.preview);
        return -1;
    }
    l->items = grown;
    l->items[l->len++] = si;
    return 0;
}

static int info_cmp(const void *a, const void *b)
{
    const SessionInfo *x = a;
    const SessionInfo *y = b;
    if (x->updated_at > y->updated_at) {
        return -1; /* absteigend: juengste zuerst */
    }
    if (x->updated_at < y->updated_at) {
        return 1;
    }
    return strcmp(y->id, x->id);
}

int session_list_load(SessionList *out, const char *dir_filter)
{
    memset(out, 0, sizeof *out);
    if (session_dir_ensure() != 0) {
        return -1;
    }
    /* NULL = aktuelles verzeichnis; ein leerer filter wuerde nichts
     * treffen und ist kein sinnvoller zustand */
    char *want = (dir_filter != NULL) ? dup_str(dir_filter) : cwd_current();
    if (want == NULL || want[0] == '\0') {
        free(want);
        return -1;
    }
    char *dir = sessions_dir();
    if (dir == NULL) {
        free(want);
        return -1;
    }
    DIR *d = opendir(dir);
    if (d == NULL) {
        free(dir);
        free(want);
        return -1;
    }

    struct dirent *e;
    int rc = 0;
    while (rc == 0 && (e = readdir(d)) != NULL) {
        size_t len = strlen(e->d_name);
        /* nur meta-dateien: ".json" am ende – ".jsonl" endet auf
         * 'l' und faellt dadurch raus */
        if (len < strlen(".json") + 1 ||
            strcmp(e->d_name + len - strlen(".json"), ".json") != 0) {
            continue;
        }
        char *path = malloc(strlen(dir) + 1 + len + 1);
        if (path == NULL) {
            rc = -1;
            break;
        }
        (void)snprintf(path, strlen(dir) + 1 + len + 1, "%s/%s", dir,
                       e->d_name);
        cJSON *meta = parse_json_file(path);
        free(path);
        if (meta != NULL) {
            /* ordner-filter: nur metas, deren cwd dem geforderten
             * verzeichnis entspricht. sessionen ohne cwd (legacy,
             * noch nicht migratiert) tauchen nicht auf – die
             * migration beim app-start holt sie nach. */
            const cJSON *jcwd = cJSON_GetObjectItemCaseSensitive(meta, "cwd");
            bool in_dir =
                (cJSON_IsString(jcwd) && strcmp(jcwd->valuestring, want) == 0);
            if (in_dir && list_add_meta(out, meta) != 0) {
                rc = -1;
            }
            cJSON_Delete(meta);
        }
    }
    (void)closedir(d);
    free(dir);
    free(want);

    if (out->len > 1) {
        qsort(out->items, out->len, sizeof *out->items, info_cmp);
    }
    return rc;
}

void session_list_free(SessionList *l)
{
    if (l == NULL) {
        return;
    }
    for (size_t i = 0; i < l->len; i++) {
        free(l->items[i].name);
        free(l->items[i].preview);
    }
    free(l->items);
    l->items = NULL;
    l->len = 0;
}

int sessions_match(const SessionList *l, const char *search, int *out,
                   int out_max)
{
    if (l == NULL || out == NULL) {
        return 0;
    }
    int n = 0;
    size_t slen = strlen(search);
    for (size_t i = 0; i < l->len && n < out_max; i++) {
        const SessionInfo *si = &l->items[i];
        bool hit = false;
        if (si->name != NULL) {
            hit = (strncmp(si->name, search, slen) == 0);
        }
        if (!hit) {
            hit = (strncmp(si->id, search, slen) == 0);
        }
        if (!hit && si->preview != NULL) {
            hit = (strncmp(si->preview, search, slen) == 0);
        }
        if (hit) {
            out[n++] = (int)i;
        }
    }
    return n;
}