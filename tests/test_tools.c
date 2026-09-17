#define _POSIX_C_SOURCE 200809L // NOLINT(bugprone-reserved-identifier)

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "cJSON.h"
#include "test.h"
#include "tools.h"
#include "utils.h"

#define TMP_PATH "/tmp/max-agent-test-tools.txt"

int main(void)
{
    /* --- registry: name, beschreibung, gueltiges schema-json --- */
    size_t len = 0;
    const OaiTool *reg = tool_registry(&len);
    CHECK(reg != NULL);
    CHECK(len == 4);
    CHECK(tool_find("read_file") != NULL);
    CHECK(tool_find("write_file") != NULL);
    CHECK(tool_find("edit_file") != NULL);
    CHECK(tool_find("bash") != NULL);

    /* sequential-markierung: nur datei-mutierende tools */
    CHECK(tool_is_sequential("write_file"));
    CHECK(tool_is_sequential("edit_file"));
    CHECK(!tool_is_sequential("read_file"));
    CHECK(!tool_is_sequential("bash"));
    CHECK(!tool_is_sequential("gibts_nicht"));
    CHECK(!tool_is_sequential(NULL));
    CHECK(tool_find("gibts_nicht") == NULL);
    CHECK(tool_find(NULL) == NULL);
    for (size_t i = 0; i < len; i++) {
        CHECK(reg[i].function.name != NULL);
        CHECK(reg[i].function.description != NULL);
        CHECK(reg[i].function.parameters_json != NULL);
        /* die parameter-schemas muessen gueltiges json sein – die
         * app sendet sie 1:1 an die api */
        cJSON *schema = cJSON_Parse(reg[i].function.parameters_json);
        CHECK(schema != NULL);
        cJSON_Delete(schema);
    }

    /* --- read_file --- */
    CHECK(write_file(TMP_PATH, "zeile 1\nzeile 2\n",
                     strlen("zeile 1\nzeile 2\n")) == 0);
    char *r = tool_execute("read_file", "{\"path\":\"" TMP_PATH "\"}");
    CHECK(r != NULL && strcmp(r, "zeile 1\nzeile 2\n") == 0);
    free(r);

    r = tool_execute("read_file", "{\"path\":\"/tmp/ganz-sicher-nicht-da\"}");
    CHECK(r != NULL && strstr(r, "error") != NULL);
    free(r);

    /* fehlendes argument */
    r = tool_execute("read_file", "{}");
    CHECK(r != NULL && strstr(r, "error") != NULL);
    free(r);

    /* --- read_file: offset/limit-paging --- */
    CHECK(write_file(TMP_PATH, "l1\nl2\nl3\nl4\nl5\n",
                     strlen("l1\nl2\nl3\nl4\nl5\n")) == 0);

    /* fenster in der mitte: hinweis auf den rest der datei */
    r = tool_execute("read_file", "{\"path\":\"" TMP_PATH "\",\"offset\":2,"
                                  "\"limit\":2}");
    CHECK(r != NULL);
    CHECK(strncmp(r, "l2\nl3\n", 6) == 0);
    CHECK(strstr(r, "2 more lines in file. Use offset=4 to continue.") != NULL);
    free(r);

    /* bis zum dateiende gelesen: kein hinweis */
    r = tool_execute("read_file", "{\"path\":\"" TMP_PATH "\",\"offset\":3,"
                                  "\"limit\":10}");
    CHECK(r != NULL);
    CHECK(strcmp(r, "l3\nl4\nl5\n") == 0);
    free(r);

    /* offset jenseits des dateiendes -> fehler */
    r = tool_execute("read_file", "{\"path\":\"" TMP_PATH "\",\"offset\":99}");
    CHECK(r != NULL && strstr(r, "beyond end of file") != NULL);
    CHECK(strstr(r, "5 lines total") != NULL);
    free(r);

    /* ungueltige werte */
    r = tool_execute("read_file",
                     "{\"path\":\"" TMP_PATH "\",\"offset\":\"x\"}");
    CHECK(r != NULL && strstr(r, "error") != NULL);
    free(r);
    r = tool_execute("read_file", "{\"path\":\"" TMP_PATH "\",\"limit\":-2}");
    CHECK(r != NULL && strstr(r, "error") != NULL);
    free(r);
    unlink(TMP_PATH);

    /* CRLF-datei: zeilenzaehlung und paging funktionieren auch mit
     * \r\n-zeilenenden */
    CHECK(write_file(TMP_PATH, "a\r\nb\r\nc\r\n", strlen("a\r\nb\r\nc\r\n")) ==
          0);
    r = tool_execute("read_file", "{\"path\":\"" TMP_PATH "\",\"offset\":2,"
                                  "\"limit\":1}");
    CHECK(r != NULL);
    CHECK(strncmp(r, "b\r\n", 3) == 0);
    CHECK(strstr(r, "1 more lines in file. Use offset=3 to continue.") != NULL);
    free(r);
    unlink(TMP_PATH);

    /* --- read_file: truncation mit weiterlese-hinweis --- */

    /* 3000 zeilen: das zeilen-limit schlaegt zu, hinweis nennt den
     * naechsten offset */
    r = tool_execute("bash", "{\"command\":\"seq 1 3000 > " TMP_PATH "\"}");
    CHECK(r != NULL && strstr(r, "[exit: 0]") != NULL);
    free(r);
    r = tool_execute("read_file", "{\"path\":\"" TMP_PATH "\"}");
    CHECK(r != NULL);
    CHECK(strncmp(r, "1\n2\n3\n", 6) == 0);
    CHECK(strstr(r, "\n\n[Showing lines 1-2000 of 3000. "
                    "Use offset=2001 to continue.]") != NULL);
    CHECK(strstr(r, "2000\n\n\n[") != NULL); /* body endet bei zeile 2000 */
    free(r);

    /* weiterlesen ab dem hinweis: bis dateiende, kein neuer hinweis */
    r = tool_execute("read_file",
                     "{\"path\":\"" TMP_PATH "\",\"offset\":2001}");
    CHECK(r != NULL);
    CHECK(strncmp(r, "2001\n", 5) == 0);
    CHECK(strstr(r, "Use offset") == NULL);
    free(r);
    unlink(TMP_PATH);

    /* byte-limit: 100 zeilen a 601 bytes -> 85 passen ins fenster */
    r = tool_execute("bash", "{\"command\":\"for i in $(seq 1 100); do "
                             "printf '%0600d\\n' $i; done > " TMP_PATH "\"}");
    CHECK(r != NULL && strstr(r, "[exit: 0]") != NULL);
    free(r);
    r = tool_execute("read_file", "{\"path\":\"" TMP_PATH "\"}");
    CHECK(r != NULL);
    CHECK(strstr(r, "[Showing lines 1-85 of 100 (50KB limit). "
                    "Use offset=86 to continue.]") != NULL);
    free(r);
    unlink(TMP_PATH);

    /* EINE zeile ueber 50KB: bash-fallback statt stillschweigender
     * kuerzung */
    r = tool_execute("bash", "{\"command\":\"awk 'BEGIN{for(i=0;i<60000;i++) "
                             "printf \\\"x\\\"}' > " TMP_PATH "\"}");
    CHECK(r != NULL && strstr(r, "[exit: 0]") != NULL);
    free(r);
    r = tool_execute("read_file", "{\"path\":\"" TMP_PATH "\"}");
    CHECK(r != NULL);
    CHECK(strstr(r, "line 1 is larger than the 50KB limit") != NULL);
    CHECK(strstr(r, "sed -n '1p'") != NULL);
    free(r);
    unlink(TMP_PATH);

    /* --- write_file --- */
    r = tool_execute("write_file",
                     "{\"path\":\"" TMP_PATH "\",\"content\":\"neu\"}");
    CHECK(r != NULL && strstr(r, "ok") != NULL);
    free(r);
    r = tool_execute("read_file", "{\"path\":\"" TMP_PATH "\"}");
    CHECK(r != NULL && strcmp(r, "neu") == 0);
    free(r);

    r = tool_execute("write_file", "{\"path\":\"/proc/kann/nicht\"}");
    CHECK(r != NULL && strstr(r, "error") != NULL);
    free(r);

    /* --- edit_file --- */
    CHECK(write_file(TMP_PATH, "alpha\nbeta\ngamma\n",
                     strlen("alpha\nbeta\ngamma\n")) == 0);

    /* einzelner ersatz */
    r = tool_execute("edit_file",
                     "{\"path\":\"" TMP_PATH "\",\"edits\":[{"
                     "\"oldText\":\"beta\",\"newText\":\"BETA\"}]}");
    CHECK(r != NULL && strstr(r, "ok") != NULL);
    free(r);
    r = tool_execute("read_file", "{\"path\":\"" TMP_PATH "\"}");
    CHECK(r != NULL && strcmp(r, "alpha\nBETA\ngamma\n") == 0);
    free(r);

    /* mehrere edits in einem call, gegen das ORIGINAL gematcht:
     * das zweite oldText umschliesst das erste NICHT, obwohl es
     * in der datei danach liegt */
    r = tool_execute("edit_file",
                     "{\"path\":\"" TMP_PATH "\",\"edits\":[{"
                     "\"oldText\":\"gamma\",\"newText\":\"delta\"},{"
                     "\"oldText\":\"BETA\",\"newText\":\"beta\"}]}");
    CHECK(r != NULL && strstr(r, "ok") != NULL);
    free(r);
    r = tool_execute("read_file", "{\"path\":\"" TMP_PATH "\"}");
    CHECK(r != NULL && strcmp(r, "alpha\nbeta\ndelta\n") == 0);
    free(r);

    /* legacy-form: oldText/newText direkt statt edits-array */
    r = tool_execute("edit_file",
                     "{\"path\":\"" TMP_PATH "\",\"oldText\":\"delta\","
                     "\"newText\":\"gamma\"}");
    CHECK(r != NULL && strstr(r, "ok") != NULL);
    free(r);

    /* nicht gefunden -> fehler als text, datei unangetastet */
    r = tool_execute("edit_file",
                     "{\"path\":\"" TMP_PATH "\",\"edits\":[{"
                     "\"oldText\":\"gibtsnicht\",\"newText\":\"x\"}]}");
    CHECK(r != NULL && strstr(r, "error") != NULL);
    CHECK(strstr(r, "exactly") != NULL);
    free(r);

    /* mehrdeutiges oldText -> abbruch */
    CHECK(write_file(TMP_PATH, "x\nx\n", strlen("x\nx\n")) == 0);
    r = tool_execute("edit_file", "{\"path\":\"" TMP_PATH "\",\"edits\":[{"
                                  "\"oldText\":\"x\",\"newText\":\"y\"}]}");
    CHECK(r != NULL && strstr(r, "occurrences") != NULL);
    free(r);
    r = tool_execute("read_file", "{\"path\":\"" TMP_PATH "\"}");
    CHECK(r != NULL && strcmp(r, "x\nx\n") == 0);
    free(r);

    /* ueberlappende edits -> abbruch */
    CHECK(write_file(TMP_PATH, "aaa bbb ccc\n", strlen("aaa bbb ccc\n")) == 0);
    r = tool_execute("edit_file",
                     "{\"path\":\"" TMP_PATH "\",\"edits\":[{"
                     "\"oldText\":\"aaa bbb\",\"newText\":\"A\"},{"
                     "\"oldText\":\"bbb ccc\",\"newText\":\"B\"}]}");
    CHECK(r != NULL && strstr(r, "overlap") != NULL);
    free(r);

    /* identischer inhalt -> kein versuchter schreibvorgang */
    CHECK(write_file(TMP_PATH, "gleich\n", strlen("gleich\n")) == 0);
    r = tool_execute("edit_file",
                     "{\"path\":\"" TMP_PATH "\",\"edits\":[{"
                     "\"oldText\":\"gleich\",\"newText\":\"gleich\"}]}");
    CHECK(r != NULL && strstr(r, "no changes") != NULL);
    free(r);

    /* leeres oldText, fehlende argumente */
    r = tool_execute("edit_file", "{\"path\":\"" TMP_PATH "\",\"edits\":[{"
                                  "\"oldText\":\"\",\"newText\":\"x\"}]}");
    CHECK(r != NULL && strstr(r, "error") != NULL);
    free(r);
    r = tool_execute("edit_file", "{\"path\":\"" TMP_PATH "\"}");
    CHECK(r != NULL && strstr(r, "error") != NULL);
    free(r);
    r = tool_execute("edit_file", "{}");
    CHECK(r != NULL && strstr(r, "error") != NULL);
    free(r);

    /* CRLF-datei: matcht in LF-form, schreibt CRLF zurueck */
    CHECK(write_file(TMP_PATH, "eins\r\nzwei\r\n",
                     strlen("eins\r\nzwei\r\n")) == 0);
    r = tool_execute("edit_file",
                     "{\"path\":\"" TMP_PATH "\",\"edits\":[{"
                     "\"oldText\":\"zwei\",\"newText\":\"drei\"}]}");
    CHECK(r != NULL && strstr(r, "ok") != NULL);
    free(r);
    r = tool_execute("read_file", "{\"path\":\"" TMP_PATH "\"}");
    /* read_file liefert die rohen bytes: CRLF bleibt erhalten */
    CHECK(r != NULL && strcmp(r, "eins\r\ndrei\r\n") == 0);
    free(r);

    unlink(TMP_PATH);

    /* --- bash --- */
    r = tool_execute("bash", "{\"command\":\"echo hallo\"}");
    CHECK(r != NULL && strstr(r, "hallo") != NULL);
    CHECK(r != NULL && strstr(r, "[exit: 0]") != NULL);
    free(r);

    /* fehlschlagendes kommando: exit-code muss erkennbar sein */
    r = tool_execute("bash", "{\"command\":\"exit 3\"}");
    CHECK(r != NULL && strstr(r, "[exit: 3]") != NULL);
    free(r);

    /* stderr wird eingesammelt */
    r = tool_execute("bash", "{\"command\":\"echo oops 1>&2\"}");
    CHECK(r != NULL && strstr(r, "oops") != NULL);
    free(r);

    /* timeout-parameter: laeuft ab -> kill, hinweis im ergebnis.
     * laufzeit des tests: gut 1 s */
    r = tool_execute("bash", "{\"command\":\"sleep 5\",\"timeout\":1}");
    CHECK(r != NULL && strstr(r, "[timeout after 1s]") != NULL);
    CHECK(r != NULL && strstr(r, "[exit: ") != NULL);
    free(r);

    /* timeout-validation */
    r = tool_execute("bash", "{\"command\":\"true\",\"timeout\":\"x\"}");
    CHECK(r != NULL && strstr(r, "error") != NULL);
    free(r);
    r = tool_execute("bash", "{\"command\":\"true\",\"timeout\":-1}");
    CHECK(r != NULL && strstr(r, "error") != NULL);
    free(r);
    r = tool_execute("bash", "{\"command\":\"true\",\"timeout\":99999999}");
    CHECK(r != NULL && strstr(r, "error") != NULL);
    free(r);

    /* tail-truncation: 3000 zeilen sind zu viel, das ENDE zaehlt.
     * der body beginnt bei zeile 1001, der hinweis nennt den
     * bereich und die temp-datei mit dem vollstaendigen output */
    r = tool_execute("bash", "{\"command\":\"seq 1 3000\"}");
    CHECK(r != NULL);
    CHECK(strncmp(r, "1001\n", 5) == 0);
    CHECK(strstr(r, "[Showing lines 1001-3000 of 3000. Full output: "
                    "/tmp/max-agent-bash-") != NULL);
    CHECK(strstr(r, "\n[exit: 0]") != NULL);
    {
        /* temp-datei aus dem hinweis extrahieren: sie existiert und
         * enthaelt den VOLLSTaendigen output ab zeile 1 */
        char full[128];
        const char *p = strstr(r, "Full output: ");
        CHECK(p != NULL);
        size_t n = 0;
        for (p += strlen("Full output: ");
             *p != ']' && *p != '\0' && n < sizeof full - 1; p++) {
            full[n++] = *p;
        }
        full[n] = '\0';
        CHECK(n > 0);
        char *body = NULL;
        size_t size = 0;
        CHECK(read_file(full, &body, &size) == 0);
        CHECK(body != NULL && strncmp(body, "1\n2\n3\n", 6) == 0);
        free(body);
        unlink(full);
    }
    free(r);

    /* tail-truncation: EINE zeile ueber 50KB -> nur ihr ende, mit
     * sonderhinweis */
    r = tool_execute("bash", "{\"command\":\"awk 'BEGIN{for(i=0;i<60000;i++) "
                             "printf \\\"x\\\"}'\"}");
    CHECK(r != NULL);
    CHECK(strstr(r, "[Showing the last 50KB of line 1. Full output: ") != NULL);
    CHECK(strstr(r, "\n[exit: 0]") != NULL);
    {
        char full[128];
        const char *p = strstr(r, "Full output: ");
        CHECK(p != NULL);
        size_t n = 0;
        for (p += strlen("Full output: ");
             *p != ']' && *p != '\0' && n < sizeof full - 1; p++) {
            full[n++] = *p;
        }
        full[n] = '\0';
        unlink(full);
    }
    free(r);

    /* --- fehlerfaelle: alles kommt als text zurueck --- */
    r = tool_execute("gibts_nicht", "{}");
    CHECK(r != NULL && strstr(r, "unknown tool") != NULL);
    free(r);

    r = tool_execute("bash", "kein json");
    CHECK(r != NULL && strstr(r, "not valid json") != NULL);
    free(r);

    r = tool_execute("bash", "{}");
    CHECK(r != NULL && strstr(r, "error") != NULL);
    free(r);

    r = tool_execute(NULL, "{}");
    CHECK(r != NULL && strstr(r, "error") != NULL);
    free(r);

    return test_report();
}