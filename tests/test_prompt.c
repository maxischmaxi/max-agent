#define _POSIX_C_SOURCE 200809L // NOLINT(bugprone-reserved-identifier)

#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "prompt.h"
#include "test.h"

int main(void)
{
    /* --- default: NULL-cfg und cfg ohne system_prompt --- */
    Config empty = {0};

    char *p = prompt_build(NULL);
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

    p = prompt_build(&empty);
    CHECK(p != NULL && strstr(p, today) != NULL);
    free(p);

    /* --- custom prompt: exakt der text, keine template-ersetzung --- */
    Config custom = {0};
    custom.system_prompt = "du bist ein test-agent. $DATE bleibt so.";
    p = prompt_build(&custom);
    CHECK(p != NULL &&
          strcmp(p, "du bist ein test-agent. $DATE bleibt so.") == 0);
    free(p);

    /* leerer string: explizit KEIN system-prompt */
    Config off = {0};
    off.system_prompt = "";
    CHECK(prompt_build(&off) == NULL);

    return test_report();
}