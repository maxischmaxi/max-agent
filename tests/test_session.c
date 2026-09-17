#define _POSIX_C_SOURCE 200809L // NOLINT(bugprone-reserved-identifier)

/* session-tests. damit nichts die echte config des benutzers
 * anfasst, zeigt HOME fuer die laufzeit des tests auf ein
 * temporaeres verzeichnis. */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "chat.h"
#include "config.h"
#include "context.h"
#include "session.h"
#include "test.h"
#include "utils.h"

static char g_home[4096];

static void test_fresh_home(void)
{
    char tmpl[] = "/tmp/maxagent-test-XXXXXX";
    if (mkdtemp(tmpl) == NULL) {
        fprintf(stderr, "mkdtemp failed\n");
        exit(1);
    }
    (void)snprintf(g_home, sizeof g_home, "%s", tmpl);
    (void)setenv("HOME", g_home, 1);
}

static void test_setup(void)
{
    test_fresh_home();
}

/* helper: die drei datenstroeme, die eine session nach einem
 * end/erneuten open wiederherstellen muessen */
static void fill_session(Session *s)
{
    Config cfg = {0};
    CHECK(session_start(s, &cfg) == 0);
    CHECK(s->active);
    CHECK(s->id[0] == 's');
    CHECK(s->messages == 0);

    CHECK(session_log_user(s, "hallo welt") == 0);
    CHECK(session_log_assistant(s, "hi!", NULL, 0, 12, 34, 5000, 0,
                                "test-model", 10, 5, false) == 0);

    ChatToolCall call = {0};
    call.id = dup_str("call_1");
    call.name = dup_str("bash");
    call.arguments = dup_str("{\"command\":\"true\"}");
    CHECK(session_log_assistant(s, "", &call, 1, 1, 2, 6000, 1, "test-model",
                                -1, -1, false) == 0);
    CHECK(session_log_tool(s, "call_1", "bash", "exit 0", 3) == 0);
    CHECK(session_log_error(s, "http 418: ich bin eine teekanne", 418) == 0);
    CHECK(session_log_notice(s, "abgebrochen") == 0);
    free(call.id);
    free(call.name);
    free(call.arguments);

    CHECK(s->messages == 6);
    CHECK(s->updated_at >= s->created_at);
}

static void test_lifecycle(void)
{
    Session s = {0};
    fill_session(&s);
    char id[SESSION_ID_MAX];
    (void)snprintf(id, sizeof id, "%s", s.id);

    session_end(&s);
    CHECK(!s.active);
    CHECK(s.id[0] == '\0');
    CHECK(s.log == NULL);

    /* wieder oeffnen: meta muss wiederkommen, transcript davor
     * unangetastet bleiben */
    CHECK(session_open(&s, id) == 0);
    CHECK(s.active);
    CHECK(strcmp(s.id, id) == 0);
    CHECK(s.messages == 6);

    /* dieselbe id nochmal oeffnen: kein fehler, keine neue datei */
    CHECK(session_open(&s, id) == 0);
    CHECK(strcmp(s.id, id) == 0);

    /* ungueltiges format: die offene session bleibt unangetastet –
     * eine kaputte id soll nie die laufende unterhaltung killen */
    CHECK(session_open(&s, "s-nicht-da") == -1);
    CHECK(s.active);

    /* gueltiges format, aber nicht vorhanden: die offene session
     * wird (wie beim resume) sauber beendet, danach scheitert das
     * oeffnen am fehlenden meta */
    CHECK(session_open(&s, "s-1a2b3c4d5e-000000") == -1);
    CHECK(!s.active);

    session_free(&s);
}

static void test_transcript_replay(void)
{
    Session s = {0};
    fill_session(&s);
    char id[SESSION_ID_MAX];
    (void)snprintf(id, sizeof id, "%s", s.id);

    session_end(&s);
    CHECK(session_open(&s, id) == 0);

    Chat chat = {0};
    CtxUsage ctx = {0};
    CHECK(session_read_transcript(&s, &chat, &ctx) == 0);

    /* 6 zeilen: user, assistant, assistant(+call), tool, error,
     * notice */
    CHECK(chat.len == 6);
    CHECK(chat.msgs[0].role == CHAT_ROLE_USER);
    CHECK(strcmp(chat.msgs[0].text, "hallo welt") == 0);
    CHECK(chat.msgs[1].role == CHAT_ROLE_ASSISTANT);
    CHECK(strcmp(chat.msgs[1].text, "hi!") == 0);
    CHECK(chat.msgs[2].role == CHAT_ROLE_ASSISTANT);
    CHECK(chat.msgs[2].tool_calls_len == 1);
    CHECK(strcmp(chat.msgs[2].tool_calls[0].id, "call_1") == 0);
    CHECK(strcmp(chat.msgs[2].tool_calls[0].name, "bash") == 0);
    CHECK(strcmp(chat.msgs[2].tool_calls[0].arguments,
                 "{\"command\":\"true\"}") == 0);
    CHECK(chat.msgs[3].role == CHAT_ROLE_TOOL);
    CHECK(chat.msgs[3].tool_call_id != NULL);
    CHECK(strcmp(chat.msgs[3].tool_call_id, "call_1") == 0);
    CHECK(strcmp(chat.msgs[3].text, "exit 0") == 0);
    CHECK(chat.msgs[4].role == CHAT_ROLE_ERROR);
    CHECK(chat.msgs[5].role == CHAT_ROLE_NOTICE);

    /* ctx-gesamtzaehler aus den assistant-ereignissen */
    CHECK(ctx.total_prompt == 10);
    CHECK(ctx.total_completion == 5);

    chat_free(&chat);

    /* abgerissene letzte zeile: anhaengen ohne newline, replay
     * muss sie ignorieren und die davor behalten */
    char path[4096];
    (void)snprintf(path, sizeof path, "%s/.config/.maxagent/sessions/%s.jsonl",
                   g_home, id);
    FILE *f = fopen(path, "a");
    CHECK(f != NULL);
    if (f != NULL) {
        (void)fputs("{\"type\":\"notice\",\"text\":\"abgerissen\"", f);
        (void)fclose(f);
    }
    Chat chat2 = {0};
    CHECK(session_read_transcript(&s, &chat2, NULL) == 0);
    CHECK(chat2.len == 6);
    chat_free(&chat2);

    /* "crash + neustart": session schliessen und wieder oeffnen.
     * dabei wird die abgerissene zeile mit einem newline abge-
     * schlossen (heal_torn_log), der naechste append verschmilzt
     * nicht mit ihr, und replay ueberspringt sie als kaputtes json. */
    session_end(&s);
    CHECK(session_open(&s, id) == 0);
    CHECK(session_log_user(&s, "nochmal") == 0);
    Chat chat3 = {0};
    CHECK(session_read_transcript(&s, &chat3, NULL) == 0);
    CHECK(chat3.len == 7);
    CHECK(strcmp(chat3.msgs[6].text, "nochmal") == 0);
    chat_free(&chat3);

    session_free(&s);
}

/* compaction-ereignis: log + replay. beim replay landet die letzte
 * summary samt watermark im ctx – aeltere compactionen werden
 * dabei ersetzt (es gibt immer nur die juengste). */
static void test_compaction_replay(void)
{
    Session s = {0};
    fill_session(&s);

    CHECK(session_log_compaction(&s, "erste zusammenfassung", 3) == 0);
    CHECK(session_log_compaction(&s, "zweite zusammenfassung", 5) == 0);

    char id[SESSION_ID_MAX];
    (void)snprintf(id, sizeof id, "%s", s.id);
    session_end(&s);
    CHECK(session_open(&s, id) == 0);

    Chat chat = {0};
    CtxUsage ctx = {0};
    CHECK(session_read_transcript(&s, &chat, &ctx) == 0);

    /* die compaction ist KEINE chat-nachricht: der verlauf bleibt
     * unberuehrt (6 zeilen wie in test_transcript_replay), aber der
     * ctx traegt summary + watermark */
    CHECK(chat.len == 6);
    CHECK(ctx.summary != NULL);
    CHECK(strcmp(ctx.summary, "zweite zusammenfassung") == 0);
    CHECK(ctx.covered == 5);
    CHECK(ctx.compact_failed == false);

    ctx_reset(&ctx);
    CHECK(ctx.summary == NULL);
    chat_free(&chat);

    /* ctx == NULL (niemand will die summary): replay laeuft trotzdem */
    Chat chat2 = {0};
    CHECK(session_read_transcript(&s, &chat2, NULL) == 0);
    CHECK(chat2.len == 6);
    chat_free(&chat2);

    session_free(&s);
}

static void test_rename(void)
{
    Session s = {0};
    Config cfg = {0};
    CHECK(session_start(&s, &cfg) == 0);

    /* leerer name ist fehler */
    CHECK(session_rename(&s, "") == -1);
    CHECK(session_rename(&s, NULL) == -1);

    CHECK(session_rename(&s, "bugfix-parser") == 0);
    char id[SESSION_ID_MAX];
    (void)snprintf(id, sizeof id, "%s", s.id);
    session_end(&s);

    /* der name ueberlebt das schliessen und neu-oeffnen */
    CHECK(session_open(&s, id) == 0);
    CHECK(s.name != NULL);
    if (s.name != NULL) {
        CHECK(strcmp(s.name, "bugfix-parser") == 0);
    }

    /* umbenennen einer geschlossenen session ist fehler */
    session_end(&s);
    CHECK(session_rename(&s, "geht nicht") == -1);
}

static void test_noop_without_session(void)
{
    Session s = {0};
    CHECK(!s.active);
    /* alle log-funktionen sind no-ops, kein crash, kein return -1
     * ausser bei NULL-json (das ist ein interner fehler, hier
     * uninteressant) */
    CHECK(session_log_user(&s, "text") == 0);
    CHECK(session_log_assistant(&s, "text", NULL, 0, -1, -1, -1, -1, NULL, -1,
                                -1, false) == 0);
    CHECK(session_log_tool(&s, "call", "bash", "out", 1) == 0);
    CHECK(session_log_error(&s, "kaputt", 500) == 0);
    CHECK(session_log_notice(&s, "hinweis") == 0);
    session_free(&s);
}

/* ------------------------------------------------------------------ */
/* ordner-bindung: sessionen gehoeren zum verzeichnis, in dem sie    */
/* gestartet wurden. der resume-dialog zeigt nur die eigenen.        */
/* ------------------------------------------------------------------ */

/* in ein verzeichnis wechseln (chdir) – rueckgabepuffer gehoert dem
 * aufrufer, damit der test zurueckwechseln kann */
static char *enter_dir(const char *dir)
{
    char *old = getcwd(NULL, 0);
    if (chdir(dir) != 0) {
        free(old);
        return NULL;
    }
    return old;
}

static void test_cwd_binding(void)
{
    test_fresh_home();

    /* ordner A und B im wegwerf-home */
    char dir_a[4096];
    char dir_b[4096];
    (void)snprintf(dir_a, sizeof dir_a, "%s/a", g_home);
    (void)snprintf(dir_b, sizeof dir_b, "%s/b", g_home);
    CHECK(mkdir(dir_a, 0755) == 0);
    CHECK(mkdir(dir_b, 0755) == 0);

    Config cfg = {0};
    char ids[2][SESSION_ID_MAX];

    /* eine session in A anfangen */
    char *old = enter_dir(dir_a);
    CHECK(old != NULL);
    if (old == NULL) {
        return;
    }
    {
        Session s = {0};
        CHECK(session_start(&s, &cfg) == 0);
        CHECK(session_log_user(&s, "frage aus ordner a") == 0);
        (void)snprintf(ids[0], SESSION_ID_MAX, "%s", s.id);
        session_end(&s);
        CHECK(s.cwd == NULL); /* end gibt die felder frei */
    }
    CHECK(chdir(old) == 0);

    /* eine session in B anfangen */
    CHECK(chdir(dir_b) == 0);
    {
        Session s = {0};
        CHECK(session_start(&s, &cfg) == 0);
        CHECK(session_log_user(&s, "frage aus ordner b") == 0);
        (void)snprintf(ids[1], SESSION_ID_MAX, "%s", s.id);
        session_end(&s);
    }
    CHECK(chdir(old) == 0);
    free(old);

    /* liste in A: nur session A */
    CHECK(chdir(dir_a) == 0);
    SessionList list = {0};
    CHECK(session_list_load(&list, NULL) == 0);
    CHECK(list.len == 1);
    if (list.len == 1) {
        CHECK(strcmp(list.items[0].id, ids[0]) == 0);
        CHECK(list.items[0].preview != NULL);
        if (list.items[0].preview != NULL) {
            CHECK(strcmp(list.items[0].preview, "frage aus ordner a") == 0);
        }
    }
    session_list_free(&list);

    /* liste in B: nur session B */
    CHECK(chdir(dir_b) == 0);
    CHECK(session_list_load(&list, NULL) == 0);
    CHECK(list.len == 1);
    if (list.len == 1) {
        CHECK(strcmp(list.items[0].id, ids[1]) == 0);
    }
    session_list_free(&list);

    /* expliziter filter statt chdir: dasselbe ergebnis */
    CHECK(chdir("/") == 0);
    CHECK(session_list_load(&list, dir_a) == 0);
    CHECK(list.len == 1);
    if (list.len == 1) {
        CHECK(strcmp(list.items[0].id, ids[0]) == 0);
    }
    session_list_free(&list);
    CHECK(session_list_load(&list, dir_b) == 0);
    CHECK(list.len == 1);
    session_list_free(&list);

    /* ein pfad, in dem nie eine session lief: leere liste, kein
     * fehler */
    CHECK(session_list_load(&list, "/gibts/hoffentlich/nicht") == 0);
    CHECK(list.len == 0);
    session_list_free(&list);

    /* resume einer session aus A bleibt moeglich, egal wo man
     * gerade ist: das oeffnen selbst kennt keinen filter (nur die
     * liste). die session behaelt ihr cwd. */
    Session s = {0};
    CHECK(session_open(&s, ids[0]) == 0);
    CHECK(s.cwd != NULL);
    if (s.cwd != NULL) {
        CHECK(strcmp(s.cwd, dir_a) == 0);
    }
    session_free(&s);
}

/* ------------------------------------------------------------------ */
/* migration: version-1-metas (ohne "cwd") bekommen beim app-start    */
/* das aktuelle arbeitsverzeichnis.                                   */
/* ------------------------------------------------------------------ */

static void test_migration(void)
{
    test_fresh_home();

    /* ein version-1-meta von hand bauen: so sahen die dateien vor
     * der ordner-bindung aus */
    char path[4096];
    (void)snprintf(path, sizeof path, "%s/.config/.maxagent/sessions", g_home);
    CHECK(mkdir_p(path, 0755) == 0);
    (void)snprintf(path, sizeof path,
                   "%s/.config/.maxagent/sessions/s-1a2b3c4d5e-000001.json",
                   g_home);
    const char *meta1 = "{\"id\":\"s-1a2b3c4d5e-000001\",\"name\":null,"
                        "\"created_at\":1,\"updated_at\":2,\"worked_ms\":0,"
                        "\"messages\":0,\"model\":null,\"base_url\":null,"
                        "\"system_prompt\":null,\"version\":\"1\"}";
    CHECK(write_file(path, meta1, strlen(meta1)) == 0);

    /* ein zweites, schon migratiertes meta: darf nicht angefasst
     * werden */
    char path2[4096];
    (void)snprintf(path2, sizeof path2,
                   "%s/.config/.maxagent/sessions/s-1a2b3c4d5e-000002.json",
                   g_home);
    const char *meta2 = "{\"id\":\"s-1a2b3c4d5e-000002\",\"name\":\"fertig\","
                        "\"cwd\":\"/alt\",\"created_at\":1,\"updated_at\":2,"
                        "\"messages\":0,\"model\":null,\"base_url\":null,"
                        "\"system_prompt\":null,\"version\":\"2\"}";
    CHECK(write_file(path2, meta2, strlen(meta2)) == 0);

    /* migration im arbeitsverzeichnis laufen lassen */
    char cwd[4096];
    CHECK(getcwd(cwd, sizeof cwd) != NULL);
    CHECK(sessions_migrate_legacy() == 0);

    /* das erste meta hat jetzt cwd + version 2 */
    cJSON *meta = parse_json_file(path);
    CHECK(meta != NULL);
    if (meta != NULL) {
        const cJSON *jcwd = cJSON_GetObjectItemCaseSensitive(meta, "cwd");
        CHECK(cJSON_IsString(jcwd));
        if (cJSON_IsString(jcwd)) {
            CHECK(strcmp(jcwd->valuestring, cwd) == 0);
        }
        const cJSON *jver = cJSON_GetObjectItemCaseSensitive(meta, "version");
        CHECK(cJSON_IsString(jver));
        if (cJSON_IsString(jver)) {
            CHECK(strcmp(jver->valuestring, SESSION_VERSION) == 0);
        }
        cJSON_Delete(meta);
    }

    /* das zweite bleibt auf /alt – die migration ist idempotent
     * und ueberschreibt keine echte ordner-bindung */
    meta = parse_json_file(path2);
    CHECK(meta != NULL);
    if (meta != NULL) {
        const cJSON *jcwd = cJSON_GetObjectItemCaseSensitive(meta, "cwd");
        CHECK(cJSON_IsString(jcwd));
        if (cJSON_IsString(jcwd)) {
            CHECK(strcmp(jcwd->valuestring, "/alt") == 0);
        }
        cJSON_Delete(meta);
    }

    /* nochmal laufen lassen: aendert nichts (idempotenz) */
    CHECK(sessions_migrate_legacy() == 0);
    meta = parse_json_file(path);
    CHECK(meta != NULL);
    if (meta != NULL) {
        const cJSON *jcwd = cJSON_GetObjectItemCaseSensitive(meta, "cwd");
        CHECK(cJSON_IsString(jcwd));
        if (cJSON_IsString(jcwd)) {
            CHECK(strcmp(jcwd->valuestring, cwd) == 0);
        }
        cJSON_Delete(meta);
    }

    /* die migratierte session erscheint jetzt in der liste des
     * ordners, das andere nicht */
    SessionList list = {0};
    CHECK(session_list_load(&list, NULL) == 0);
    CHECK(list.len == 1);
    if (list.len == 1) {
        CHECK(strcmp(list.items[0].id, "s-1a2b3c4d5e-000001") == 0);
    }
    session_list_free(&list);
}

static void test_list_and_match(void)
{
    /* eigenes home: fruehere tests haben hier schon sessionen
     * angelegt, die zaehlen fuer die listen-pruefung nicht mit */
    test_fresh_home();

    /* drei sessionen im aktuellen ordner, die mittlere benannt */
    Session s = {0};
    Config cfg = {0};

    SessionInfo before[3];
    int n_before = 0;

    CHECK(session_start(&s, &cfg) == 0);
    (void)session_log_user(&s, "erste frage");
    (void)snprintf(before[n_before].id, SESSION_ID_MAX, "%s", s.id);
    n_before++;
    session_end(&s);

    CHECK(session_start(&s, &cfg) == 0);
    CHECK(session_rename(&s, "meine lieblingssession") == 0);
    (void)snprintf(before[n_before].id, SESSION_ID_MAX, "%s", s.id);
    n_before++;
    session_end(&s);

    CHECK(session_start(&s, &cfg) == 0);
    (void)session_log_user(&s, "anderes thema");
    (void)snprintf(before[n_before].id, SESSION_ID_MAX, "%s", s.id);
    n_before++;
    session_end(&s);

    SessionList list = {0};
    CHECK(session_list_load(&list, NULL) == 0);
    CHECK(list.len == 3);

    /* jede session ist in der liste, egal in welcher reihenfolge */
    for (int i = 0; i < n_before; i++) {
        bool found = false;
        for (size_t j = 0; j < list.len; j++) {
            if (strcmp(list.items[j].id, before[i].id) == 0) {
                found = true;
                break;
            }
        }
        CHECK(found);
    }

    /* sortierung: absteigend nach updated_at */
    for (size_t i = 1; i < list.len; i++) {
        CHECK(list.items[i - 1].updated_at >= list.items[i].updated_at);
    }

    /* name und preview */
    bool named = false;
    bool previewed = false;
    for (size_t i = 0; i < list.len; i++) {
        if (list.items[i].name != NULL) {
            CHECK(strcmp(list.items[i].name, "meine lieblingssession") == 0);
            named = true;
        }
        if (list.items[i].preview != NULL) {
            previewed = true;
        }
    }
    CHECK(named);
    CHECK(previewed);

    /* messages-zaehler aus dem meta */
    bool counted = false;
    for (size_t i = 0; i < list.len; i++) {
        if (strcmp(list.items[i].id, before[0].id) == 0) {
            CHECK(list.items[i].messages == 1);
            counted = true;
        }
    }
    CHECK(counted);

    /* treffer-filter: name, id, preview, leer = alles */
    int hits[DIALOG_MATCH_MAX];
    int n = sessions_match(&list, "", hits, DIALOG_MATCH_MAX);
    CHECK(n == 3);

    n = sessions_match(&list, "meine", hits, DIALOG_MATCH_MAX);
    CHECK(n == 1);
    CHECK(strcmp(list.items[hits[0]].name, "meine lieblingssession") == 0);

    n = sessions_match(&list, before[1].id, hits, DIALOG_MATCH_MAX);
    CHECK(n == 1);

    n = sessions_match(&list, "erste", hits, DIALOG_MATCH_MAX);
    CHECK(n == 1);
    if (n == 1) {
        CHECK(list.items[hits[0]].preview != NULL);
    }

    CHECK(sessions_match(&list, "gibtsnicht", hits, DIALOG_MATCH_MAX) == 0);

    /* out_max begrenzt */
    int small[1];
    CHECK(sessions_match(&list, "", small, 1) == 1);

    session_list_free(&list);
    CHECK(list.len == 0);
    CHECK(list.items == NULL);

    /* free zweimal ist ok (dialog schliessen + app-ende) */
    session_list_free(&list);
}

static void test_unique_ids(void)
{
    test_fresh_home();
    Config cfg = {0};
    char first[SESSION_ID_MAX] = {0};
    for (int i = 0; i < 8; i++) {
        Session s = {0};
        CHECK(session_start(&s, &cfg) == 0);
        if (i == 0) {
            (void)snprintf(first, sizeof first, "%s", s.id);
        } else {
            CHECK(strcmp(s.id, first) != 0);
        }
        session_end(&s);
    }
}

int main(void)
{
    test_setup();

    test_lifecycle();
    test_transcript_replay();
    test_compaction_replay();
    test_rename();
    test_noop_without_session();
    test_cwd_binding();
    test_migration();
    test_list_and_match();
    test_unique_ids();

    session_list_free(&(SessionList){0});
    return test_report();
}