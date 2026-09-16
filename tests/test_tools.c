#define _POSIX_C_SOURCE 200809L // NOLINT(bugprone-reserved-identifier)

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "cJSON.h"
#include "test.h"
#include "tools.h"
#include "utils.h"

#define TMP_PATH "/tmp/max-agent-test-tools.txt"

/* welche tools vorgelegt werden muessen: lesen darf durchlaufen,
 * alles andere nicht. unbekanntes gilt als gefaehrlich. */
static void test_needs_confirm(void)
{
    CHECK(!tool_needs_confirm("read_file")); /* nur lesen */
    CHECK(tool_needs_confirm("write_file"));
    CHECK(tool_needs_confirm("bash"));
    CHECK(tool_needs_confirm("gibts-nicht")); /* im zweifel fragen */
    CHECK(tool_needs_confirm(NULL));

    /* jedes registrierte tool hat eine klare antwort, und alles
     * ausser read_file wird vorgelegt */
    size_t len = 0;
    const OaiTool *tools = tool_registry(&len);
    CHECK(len > 0);
    for (size_t i = 0; i < len; i++) {
        const char *name = tools[i].function.name;
        CHECK(name != NULL);
        if (strcmp(name, "read_file") != 0) {
            CHECK(tool_needs_confirm(name));
        }
    }
}

int main(void)
{
    test_needs_confirm();

    /* --- registry: name, beschreibung, gueltiges schema-json --- */
    size_t len = 0;
    const OaiTool *reg = tool_registry(&len);
    CHECK(reg != NULL);
    CHECK(len == 3);
    CHECK(tool_find("read_file") != NULL);
    CHECK(tool_find("write_file") != NULL);
    CHECK(tool_find("bash") != NULL);
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