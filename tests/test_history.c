#include <stdio.h>
#include <string.h>

#include "history.h"
#include "test.h"

static void test_add(void)
{
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

int main(void)
{
    test_add();
    test_ring_full();
    test_browse();
    test_draft();
    test_empty();
    test_multiline();
    return test_report();
}
