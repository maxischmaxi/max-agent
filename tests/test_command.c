#include <string.h>

#include "command.h"
#include "test.h"

static void test_cmd_lookup(void)
{
    CHECK(cmd_lookup("/clear") == CMD_CLEAR);
    CHECK(cmd_lookup("/models") == CMD_MODELS);
    CHECK(cmd_lookup("/quit") == CMD_QUIT);

    /* /sessions ist ein alias auf den resume-dialog */
    CHECK(cmd_lookup("/sessions") == CMD_SESSIONS);
    CHECK(cmd_lookup("sessions") == CMD_SESSIONS);

    /* ohne slash auch erlaubt */
    CHECK(cmd_lookup("quit") == CMD_QUIT);

    /* exakter match: unvollstaendige/bekannte faelle */
    CHECK(cmd_lookup("/qu") == -1);
    CHECK(cmd_lookup("/quitx") == -1);
    CHECK(cmd_lookup("/help") == -1);
    CHECK(cmd_lookup("") == -1);
    CHECK(cmd_lookup(NULL) == -1);
}

static void test_cmd_match(void)
{
    int m[COMMAND_COUNT];

    /* leerer prefix: alle (inklusive alias) */
    CHECK(cmd_match("", m, COMMAND_COUNT) == COMMAND_COUNT);

    /* eindeutiger prefix */
    int n = cmd_match("q", m, COMMAND_COUNT);
    CHECK(n == 1);
    CHECK(m[0] == CMD_QUIT);

    /* vollstaendiger name matcht nur sich selbst */
    n = cmd_match("clear", m, COMMAND_COUNT);
    CHECK(n == 1);
    CHECK(m[0] == CMD_CLEAR);

    /* alias: sessions matcht CMD_SESSIONS */
    n = cmd_match("sessions", m, COMMAND_COUNT);
    CHECK(n == 1);
    CHECK(m[0] == CMD_SESSIONS);

    /* kein treffer */
    CHECK(cmd_match("zz", m, COMMAND_COUNT) == 0);
    CHECK(cmd_match("clearx", m, COMMAND_COUNT) == 0);

    /* out_max begrenzen */
    int small[1];
    CHECK(cmd_match("", small, 1) == 1);
    CHECK(small[0] == CMD_CLEAR); /* tabelle bleibt in CmdId-reihenfolge */
}

static void test_names_and_descs(void)
{
    /* jede command braucht name und beschreibung */
    for (int i = 0; i < COMMAND_COUNT; i++) {
        CHECK(COMMANDS[i].name != NULL && COMMANDS[i].name[0] != '\0');
        CHECK(COMMANDS[i].desc != NULL && COMMANDS[i].desc[0] != '\0');
    }

    CHECK(cmd_name_col() == (int)strlen(COMMAND_SETTINGS)); /* laengster name */
}

int main(void)
{
    test_cmd_lookup();
    test_cmd_match();
    test_names_and_descs();
    return test_report();
}