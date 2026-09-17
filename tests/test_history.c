#define _POSIX_C_SOURCE 200809L // NOLINT(bugprone-reserved-identifier)

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "history.h"
#include "test.h"
#include "utils.h"

/* ------------------------------------------------------------------ */
/* persistenz braucht ein wegwerf-HOME, sonst wuerde der test die    */
/* echte history des benutzers anfassen                                */
/* ------------------------------------------------------------------ */
static char g_home[512];

static void fresh_home(void)
{
    char tmpl[] = "/tmp/maxagent-history-XXXXXX";
    if (mkdtemp(tmpl) == NULL) {
        fprintf(stderr, "mkdtemp failed\n");
        exit(1);
    }
    (void)snprintf(g_home, sizeof g_home, "%s", tmpl);
    (void)setenv("HOME", g_home, 1);
}

static void history_file_write(const char *content)
{
    char dir[512];
    (void)snprintf(dir, sizeof dir, "%s/.config/.maxagent", g_home);
    CHECK(mkdir_p(dir, 0755) == 0);
    char path[600];
    (void)snprintf(path, sizeof path, "%s/.config/.maxagent/history", g_home);
    CHECK(write_file(path, content, strlen(content)) == 0);
}

static void test_add(void)
{
    fresh_home();
    History h = {0};

    /* leeres und NULL werden nicht aufgenommen */
    history_add(&h, NULL);
    history_add(&h, "");
    CHECK(h.len == 0);
    history_add(NULL, "x"); /* darf nicht knallen */

    history_add(&h, "erste");
    history_add(&h, "zweite");
    CHECK(h.len == 2);
    CHECK(strcmp(h.entries[0], "erste") == 0);  /* aeltester zuerst */
    CHECK(strcmp(h.entries[1], "zweite") == 0); /* juengster zuletzt */

    /* direkte wiederholung wird uebersprungen ... */
    history_add(&h, "zweite");
    CHECK(h.len == 2);
    /* ... eine spaetere wiederholung aber nicht */
    history_add(&h, "erste");
    CHECK(h.len == 3);
    CHECK(strcmp(h.entries[2], "erste") == 0);

    history_free(&h);
    CHECK(h.len == 0);
}

static void test_ring_full(void)
{
    /* laeuft der ring ueber, faellt der aelteste raus */
    History h = {0};
    char buf[16];
    for (int i = 0; i < HISTORY_MAX + 10; i++) {
        snprintf(buf, sizeof buf, "e%d", i);
        history_add(&h, buf);
    }
    CHECK(h.len == HISTORY_MAX);
    CHECK(strcmp(h.entries[0], "e10") == 0); /* e0..e9 verdraengt */
    CHECK(strcmp(h.entries[HISTORY_MAX - 1], "e73") == 0);

    /* blaettern bleibt auch am rand heil */
    const char *e = NULL;
    for (int i = 0; i < HISTORY_MAX + 5; i++) {
        e = history_prev(&h, NULL);
    }
    CHECK(e == NULL); /* der letzte versuch lief ins leere */
    CHECK(h.pos == HISTORY_MAX);
    CHECK(strcmp(history_next(&h), "e11") == 0);

    history_free(&h);
}

static void test_browse(void)
{
    History h = {0};
    history_add(&h, "eins");
    history_add(&h, "zwei");
    history_add(&h, "drei");

    /* rueckwaerts: juengster zuerst */
    const char *e = history_prev(&h, "entwurf");
    CHECK(e != NULL && strcmp(e, "drei") == 0);
    e = history_prev(&h, "wird ignoriert"); /* draft nur beim 1. mal */
    CHECK(e != NULL && strcmp(e, "zwei") == 0);
    e = history_prev(&h, NULL);
    CHECK(e != NULL && strcmp(e, "eins") == 0);

    /* am aeltesten ende: NULL = eingabe bleibt stehen */
    CHECK(history_prev(&h, NULL) == NULL);
    CHECK(h.pos == 3); /* position laeuft nicht weiter */

    /* vorwaerts zurueck bis zum entwurf */
    e = history_next(&h);
    CHECK(e != NULL && strcmp(e, "zwei") == 0);
    e = history_next(&h);
    CHECK(e != NULL && strcmp(e, "drei") == 0);
    e = history_next(&h);
    CHECK(e != NULL && strcmp(e, "entwurf") == 0); /* der gemerkte text */
    CHECK(h.pos == 0);                             /* blaettern beendet */

    /* darueber hinaus: nichts zu tun */
    CHECK(history_next(&h) == NULL);

    history_free(&h);
}

static void test_draft(void)
{
    History h = {0};
    history_add(&h, "alt");

    /* leeres feld: der entwurf ist "" und leert die eingabe wieder */
    CHECK(strcmp(history_prev(&h, NULL), "alt") == 0);
    const char *back = history_next(&h);
    CHECK(back != NULL && back[0] == '\0'); /* "" = feld leeren */

    /* "" zaehlt wie kein entwurf */
    CHECK(strcmp(history_prev(&h, ""), "alt") == 0);
    CHECK(h.draft == NULL);
    CHECK(history_next(&h)[0] == '\0');

    /* eine neue eingabe beendet ein laufendes blaettern */
    CHECK(strcmp(history_prev(&h, "halbfertig"), "alt") == 0);
    CHECK(h.pos == 1);
    history_add(&h, "abgeschickt");
    CHECK(h.pos == 0);
    CHECK(h.draft == NULL);

    /* history_reset beendet es ebenfalls */
    CHECK(strcmp(history_prev(&h, "neuer entwurf"), "abgeschickt") == 0);
    history_reset(&h);
    CHECK(h.pos == 0);
    CHECK(h.draft == NULL);
    CHECK(history_next(&h) == NULL);

    history_free(&h);
}

static void test_empty(void)
{
    /* ohne eintraege passiert nichts */
    History h = {0};
    CHECK(history_prev(&h, "text") == NULL);
    CHECK(history_next(&h) == NULL);
    CHECK(history_prev(NULL, "text") == NULL);
    CHECK(history_next(NULL) == NULL);
    history_reset(NULL);
    history_free(NULL);
    history_free(&h);
}

static void test_multiline(void)
{
    /* mehrzeilige eingaben ueberleben unveraendert */
    History h = {0};
    history_add(&h, "zeile eins\nzeile zwei\nzeile drei");
    const char *e = history_prev(&h, NULL);
    CHECK(e != NULL && strcmp(e, "zeile eins\nzeile zwei\nzeile drei") == 0);
    history_free(&h);
}

/* ------------------------------------------------------------------ */
/* persistenz: file schreiben beim add, file lesen beim load          */
/* ------------------------------------------------------------------ */

static void test_persist_roundtrip(void)
{
    fresh_home();
    History h = {0};
    history_load(&h); /* file fehlt noch: leer */
    CHECK(h.len == 0);

    history_add(&h, "erste frage");
    history_add(&h, "zweite\nueber mehrere zeilen");
    history_add(&h, "erste frage"); /* keine DIREKTE wiederholung (s. u.) */
    history_add(&h, "erste frage"); /* direkte wiederholung: verworfen */

    /* eine "neue app": frisches objekt, file laden. "erste frage"
     * in der mitte ist keine direkte wiederholung des juengsten
     * ("zweite...") und bleibt deshalb stehen (shell-semantik);
     * die vierte eingabe DIREKT auf dem juengsten faellt weg. */
    History h2 = {0};
    history_load(&h2);
    CHECK(h2.len == 3);
    CHECK(strcmp(h2.entries[0], "erste frage") == 0);
    CHECK(strcmp(h2.entries[1], "zweite\nueber mehrere zeilen") == 0);
    CHECK(strcmp(h2.entries[2], "erste frage") == 0);

    /* das file traegt drei zeilen, die \n der mehrzeiligen
     * eingabe ist kodiert */
    char path[600];
    (void)snprintf(path, sizeof path, "%s/.config/.maxagent/history", g_home);
    char *buf = NULL;
    size_t size = 0;
    CHECK(read_file(path, &buf, &size) == 0);
    if (buf != NULL) {
        int lines = 0;
        for (size_t i = 0; i < size; i++) {
            if (buf[i] == '\n') {
                lines++;
            }
        }
        CHECK(lines == 3);
        CHECK(strstr(buf, "zweite\\nueber") != NULL); /* \n kodiert */
        free(buf);
    }
    /* blaettern in der geladenen history funktioniert wie gehabt */
    const char *e = history_prev(&h2, NULL);
    CHECK(e != NULL && strcmp(e, "erste frage") == 0);
    e = history_prev(&h2, NULL);
    CHECK(e != NULL && strcmp(e, "zweite\nueber mehrere zeilen") == 0);
    e = history_prev(&h2, NULL);
    CHECK(e != NULL && strcmp(e, "erste frage") == 0);

    history_free(&h);
    history_free(&h2);
}

static void test_load_dedup_and_cap(void)
{
    fresh_home();
    /* file mit einer direkten wiederholung: laden dedupliziert den
     * juengsten nachbarn */
    history_file_write("a\na\nb\n");

    History h = {0};
    history_load(&h);
    CHECK(h.len == 2);
    CHECK(strcmp(h.entries[0], "a") == 0);
    CHECK(strcmp(h.entries[1], "b") == 0);
    history_free(&h);

    /* mehr als HISTORY_MAX zeilen: ring voll, aelteste raus */
    char big[(HISTORY_MAX + 16) * 8];
    size_t w = 0;
    for (int i = 0; i < HISTORY_MAX + 16; i++) {
        w += (size_t)snprintf(big + w, sizeof big - w, "z%d\n", i);
    }
    history_file_write(big);
    History h2 = {0};
    history_load(&h2);
    CHECK(h2.len == HISTORY_MAX);
    CHECK(strcmp(h2.entries[0], "z16") == 0);
    CHECK(strcmp(h2.entries[HISTORY_MAX - 1], "z79") == 0);
    history_free(&h2);
}

static void test_load_garbage(void)
{
    fresh_home();
    /* leere zeilen, muell, CRLF: laden darf nicht abstuerzen, die
     * guenstigsten zeilen ueberleben */
    history_file_write("\r\n\nkaputte zeile ohne ende");
    History h = {0};
    history_load(&h);
    CHECK(h.len == 1); /* nur die muellzeile ueberlebt */
    if (h.len == 1) {
        CHECK(strcmp(h.entries[0], "kaputte zeile ohne ende") == 0);
    }
    history_free(&h);

    /* file ohne leserechte/inhalt: leer ist ok, kein fehler */
    fresh_home();
    History h2 = {0};
    history_load(&h2);
    CHECK(h2.len == 0);
    history_free(&h2);
    CHECK(history_prev(&h2, NULL) == NULL);
}

int main(void)
{
    test_add();
    test_ring_full();
    test_browse();
    test_draft();
    test_empty();
    test_multiline();
    test_persist_roundtrip();
    test_load_dedup_and_cap();
    test_load_garbage();
    return test_report();
}