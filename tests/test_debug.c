#include <stdbool.h>
#include <string.h>

#include "debug.h"
#include "test.h"

static void test_sanitize(void)
{
    char buf[64];

    /* escape-sequenzen werden neutralisiert: die sidebar rendert
     * text und darf keine farb-codes ausgeben */
    strcpy(buf, "match=\x1b[36m");
    dbg_sanitize(buf);
    CHECK(strcmp(buf, "match=.[36m") == 0);

    strcpy(buf, "\x1b[7m\x1b[27m");
    dbg_sanitize(buf);
    CHECK(strcmp(buf, ".[7m.[27m") == 0);

    /* steuerzeichen generell */
    strcpy(buf, "a\tb\nc");
    dbg_sanitize(buf);
    CHECK(strcmp(buf, "a.b.c") == 0);

    strcpy(buf, "del\x7f");
    dbg_sanitize(buf);
    CHECK(strcmp(buf, "del.") == 0);

    /* normaler text bleibt unangetastet (inkl. utf-8) */
    strcpy(buf, "theme: auto 100%\xe2\x80\x94ok");
    dbg_sanitize(buf);
    CHECK(strcmp(buf, "theme: auto 100%\xe2\x80\x94ok") == 0);

    /* leerer string */
    strcpy(buf, "");
    dbg_sanitize(buf);
    CHECK(buf[0] == '\0');
}

#ifndef NDEBUG

static void test_dbg_logf_ring(void)
{
    /* 82KB-modulzustand: static statt stack */
    static DebugState st;

    /* frischer zustand */
    CHECK(st.count == 0 && st.head == 0);

    dbg_logf(&st, "zahl: %d, text: %s", 42, "ok");
    CHECK(st.count == 1);
    CHECK(strncmp(st.log[0], "zahl: 42", 8) == 0);

    /* escape-sequenzen landen neutralisiert im ring */
    dbg_logf(&st, "farbe: \x1b[96m und \x1b[0m");
    CHECK(st.count == 2);
    CHECK(strchr(st.log[1], '\x1b') == NULL);
    CHECK(strstr(st.log[1], ".[96m") != NULL);

    /* ring-ueberlauf: aeltester wird ueberschrieben, head wandert */
    for (int i = 0; i < DBG_MAX_LINES + 10; i++) {
        dbg_logf(&st, "zeile %d", i);
    }
    CHECK(st.count == DBG_MAX_LINES); /* count laeuft nie ueber */
    CHECK(st.head == 12);             /* 524 inserts - 512 plaetze */
    /* aeltester jetzt "zeile 10" (die ersten zwei test-logs sind weg) */
    CHECK(strncmp(st.log[st.head], "zeile 10", 8) == 0);
    /* neuester liegt direkt davor im ring */
    CHECK(strncmp(st.log[(st.head + DBG_MAX_LINES - 1) % DBG_MAX_LINES],
                  "zeile", 5) == 0);
}

#else

/* release-build: dbg_logf existiert nicht, dbg_log ist no-op */
static void test_dbg_log_noop(void)
{
    int x = 0;
    dbg_log(&x, "wird ignoriert: %d", 42);
    CHECK(true);
}

#endif /* NDEBUG */

int main(void)
{
    test_sanitize();
#ifndef NDEBUG
    test_dbg_logf_ring();
#else
    test_dbg_log_noop();
#endif
    return test_report();
}