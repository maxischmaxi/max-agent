#define _POSIX_C_SOURCE 200809L // NOLINT(bugprone-reserved-identifier)

#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "prompt.h"
#include "test.h"

int main(void)
{
    /* --- hardcoded: kein config-override mehr, immer die vorlage --- */
    char *p = prompt_build();
    /* identitaet + arbeitsregeln wie beim pi-agenten-vorbild */
    CHECK(p != NULL && strstr(p, "You are max agent") != NULL);
    CHECK(p != NULL && strstr(p, "coding agent") != NULL);
    CHECK(p != NULL && strstr(p, "Today's date:") != NULL);
    CHECK(p != NULL && strstr(p, "Working directory:") != NULL);
    CHECK(p != NULL && strstr(p, "same language") != NULL);
    CHECK(p != NULL && strstr(p, "destructive actions") != NULL);
    free(p);

    /* datum im template ist heute (yyyy-mm-dd) */
    char today[16];
    struct tm tmv;
    time_t now = time(NULL);
    CHECK(localtime_r(&now, &tmv) != NULL);
    CHECK(strftime(today, sizeof today, "%Y-%m-%d", &tmv) > 0);

    p = prompt_build();
    CHECK(p != NULL && strstr(p, today) != NULL);
    free(p);

    /* --- effizienz-regeln aus der praxis (71x dasselbe gdb-kommando,
     * ganze dateien wiederholt gelesen): jede runde kostet den
     * vollen kontext, also muss der prompt das modell bremsen --- */
    p = prompt_build();
    CHECK(p != NULL &&
          strstr(p, "Never run the exact same command twice") != NULL);
    CHECK(p != NULL && strstr(p, "failed twice") != NULL);
    CHECK(p != NULL && strstr(p, "offset and limit") != NULL);
    CHECK(p != NULL && strstr(p, "do not re-read a file") != NULL);
    CHECK(p != NULL && strstr(p, "ONE hypothesis") != NULL);
    CHECK(p != NULL && strstr(p, "Batch independent tool calls") != NULL);
    free(p);

    /* zweiter aufruf liefert dieselbe vorlage (kein zustand) */
    char *q = prompt_build();
    char *r = prompt_build();
    CHECK(q != NULL && r != NULL && strcmp(q, r) == 0);
    free(q);
    free(r);

    return test_report();
}