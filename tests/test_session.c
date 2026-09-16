#define _POSIX_C_SOURCE 200809L // NOLINT(bugprone-reserved-identifier)

/* session-tests. damit nichts die echte config des benutzers
 * anfasst, zeigt HOME fuer die laufzeit des tests auf ein
 * temporaeres verzeichnis. */
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
    CHECK(session_log_assistant(s, "hi!", NULL, 0, 12, 34, 0, "test-model", 10,
                                5, false) == 0);

    ChatToolCall call = {0};
    call.id = dup_str("call_1");
    call.name = dup_str("bash");
    call.arguments = dup_str("{\"command\":\"true\"}");
    CHECK(session_log_assistant(s, "", &call, 1, 1, 2, 1, "test-model", -1, -1,
                                false) == 0);
    CHECK(session_log_tool(s, "call_1", "bash", "exit 0", 3, "auto") == 0);
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
     * nicht mit ihr, und replay ueberspringt sie als kaputtes
     * json. */
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
    CHECK(session_log_assistant(&s, "text", NULL, 0, -1, -1, -1, NULL, -1, -1,
                                false) == 0);
    CHECK(session_log_tool(&s, "call", "bash", "out", 1, "auto") == 0);
    CHECK(session_log_error(&s, "kaputt", 500) == 0);
    CHECK(session_log_notice(&s, "hinweis") == 0);
    session_free(&s);
}

static void test_list_and_match(void)
{
    /* eigenes home: fruehere tests haben hier schon sessionen
     * angelegt, die zaehlen fuer die listen-pruefung nicht mit */
    test_fresh_home();

    /* drei sessionen, die mittlere benannt */
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
    CHECK(session_list_load(&list) == 0);
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
    test_rename();
    test_noop_without_session();
    test_list_and_match();
    test_unique_ids();

    session_list_free(&(SessionList){0});
    return test_report();
}