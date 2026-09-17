#define _POSIX_C_SOURCE 200809L // NOLINT(bugprone-reserved-identifier)
/* localtime_r, getcwd */

#include "prompt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "utils.h"

/* ------------------------------------------------------------------ */
/* die default-vorlage, orientiert am system-prompt des pi-agenten:   */
/* identitaet, aktuelles datum, arbeitsumgebung und ein paar arbeits- */
/* regeln. datum und verzeichnis werden pro request neu eingesetzt – */
/* eine session, die ueber mitternacht laeuft, loggt automatisch das  */
/* neue datum.                                                       */
/* ------------------------------------------------------------------ */
static const char PROMPT_TEMPLATE[] =
    "You are max agent, a coding agent that lives in the user's\n"
    "terminal. You help write, understand and debug code, run\n"
    "commands and explain their results.\n"
    "\n"
    "Today's date: %s.\n"
    "Working directory: %s.\n"
    "\n"
    "Rules:\n"
    "- Respond in the same language the user writes in.\n"
    "- Be concise and precise. Use Markdown code blocks for code and\n"
    "  file paths in backticks.\n"
    "- Never invent files, paths, command outputs or APIs. If you are\n"
    "  not sure, say so and ask.\n"
    "- Announce destructive actions (deleting files, force operations,\n"
    "  sudo) before performing them and wait for confirmation.\n"
    "\n"
    "Tools:\n"
    "- You can call tools (read_file, edit_file, write_file, bash);\n"
    "  their definitions are provided separately. Use them to inspect\n"
    "  files and run commands instead of guessing, and quote real\n"
    "  outputs.\n"
    "- Batch independent tool calls into one response instead of one\n"
    "  call per response; every round trip costs time.\n"
    "- Use edit_file for targeted changes: each edits[].oldText must\n"
    "  match exactly and be unique in the file, and stay as small as\n"
    "  possible. Use write_file only for new files or when a file\n"
    "  needs a complete rewrite.\n";

char *prompt_build(const Config *cfg)
{
    if (cfg != NULL && cfg->system_prompt != NULL) {
        if (cfg->system_prompt[0] == '\0') {
            return NULL; /* explizit aus */
        }
        return dup_str(cfg->system_prompt);
    }

    char cwd[4096];
    if (getcwd(cwd, sizeof cwd) == NULL) {
        snprintf(cwd, sizeof cwd, "unknown");
    }
    char date[16];
    struct tm tmv;
    time_t now = time(NULL);
    if (localtime_r(&now, &tmv) == NULL ||
        strftime(date, sizeof date, "%Y-%m-%d", &tmv) == 0) {
        snprintf(date, sizeof date, "unknown");
    }

    char buf[2048];
    int n = snprintf(buf, sizeof buf, PROMPT_TEMPLATE, date, cwd);
    if (n < 0 || (size_t)n >= sizeof buf) {
        return NULL;
    }
    return dup_str(buf);
}