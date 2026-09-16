#define _POSIX_C_SOURCE 200809L // NOLINT(bugprone-reserved-identifier)
/* popen, pclose, WEXITSTATUS */

#include "tools.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>

#include "cJSON.h"
#include "utils.h"

/* ------------------------------------------------------------------ */
/* definitionen: beschreibungen sind an die models gerichtet –         */
/* praegnend, mit klaren erwartungen an die argumente.                 */
/* ------------------------------------------------------------------ */
static const OaiTool TOOLS[] = {
    {
        .function =
            {
                .name = "read_file",
                .description =
                    "Read the complete content of a file at the given path "
                    "(relative to the working directory). Returns the file "
                    "content as text.",
                .parameters_json =
                    "{\"type\":\"object\",\"properties\":{"
                    "\"path\":{\"type\":\"string\",\"description\":"
                    "\"path of the file to read\"}},"
                    "\"required\":[\"path\"]}",
            },
    },
    {
        .function =
            {
                .name = "write_file",
                .description =
                    "Write content to a file at the given path (relative to "
                    "the working directory). Creates or overwrites the file.",
                .parameters_json =
                    "{\"type\":\"object\",\"properties\":{"
                    "\"path\":{\"type\":\"string\",\"description\":"
                    "\"path of the file to write\"},"
                    "\"content\":{\"type\":\"string\",\"description\":"
                    "\"complete new content of the file\"}},"
                    "\"required\":[\"path\",\"content\"]}",
            },
    },
    {
        .function =
            {
                .name = "bash",
                .description =
                    "Run a shell command in the working directory and return "
                    "its combined output (stdout and stderr) plus the exit "
                    "code. Use this for listing directories, searching, "
                    "building and running tests.",
                .parameters_json =
                    "{\"type\":\"object\",\"properties\":{"
                    "\"command\":{\"type\":\"string\",\"description\":"
                    "\"the shell command to run\"}},"
                    "\"required\":[\"command\"]}",
            },
    },
};
#define TOOL_COUNT ((size_t)(sizeof TOOLS / sizeof TOOLS[0]))

const OaiTool *tool_registry(size_t *len)
{
    if (len != NULL) {
        *len = TOOL_COUNT;
    }
    return TOOLS;
}

const OaiTool *tool_find(const char *name)
{
    if (name == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < TOOL_COUNT; i++) {
        if (strcmp(TOOLS[i].function.name, name) == 0) {
            return &TOOLS[i];
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* hilfsfunktionen                                                      */
/* ------------------------------------------------------------------ */

/* formatierter fehler- bzw. ergebnis-string (heap) */
static char *fmt(const char *format, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, format);
    (void)vsnprintf(buf, sizeof buf, format, ap);
    va_end(ap);
    return dup_str(buf);
}

/* string-argument aus den geparsten json-argumenten holen.
 * fehlt oder kein string: NULL (die meldung baut der aufrufer) */
static char *arg_string(const cJSON *args, const char *key)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(args, key);
    if (!cJSON_IsString(v) || v->valuestring == NULL) {
        return NULL;
    }
    return dup_str(v->valuestring);
}

/* ergebnis auf TOOL_MAX_OUT kappen (mit hinweis hinten dran, wenn
 * abgeschnitten wurde). uebernimmt den buffer, gibt ihn (ggf.
 * gekuerzt) zurueck. */
static char *cap_result(char *s)
{
    if (s == NULL) {
        return NULL;
    }
    size_t len = strlen(s);
    if (len <= TOOL_MAX_OUT) {
        return s;
    }
    char *cut = realloc(s, TOOL_MAX_OUT + 64);
    if (cut == NULL) {
        free(s);
        return NULL;
    }
    cut[TOOL_MAX_OUT] = '\0';
    (void)snprintf(cut + TOOL_MAX_OUT, 64, "\n[output truncated at %zu bytes]",
                   TOOL_MAX_OUT);
    return cut;
}

/* ------------------------------------------------------------------ */
/* die drei tools                                                       */
/* ------------------------------------------------------------------ */

static char *tool_read_file(const cJSON *args)
{
    char *path = arg_string(args, "path");
    if (path == NULL) {
        return fmt("error: missing string argument 'path'");
    }
    char *buf = NULL;
    size_t size = 0;
    if (read_file(path, &buf, &size) != 0) {
        char *err = fmt("error: cannot read '%s'", path);
        free(path);
        return err;
    }
    free(path);
    /* read_file liefert size + '\0' – als string nutzbar */
    return cap_result(buf);
}

static char *tool_write_file(const cJSON *args)
{
    char *path = arg_string(args, "path");
    char *content = arg_string(args, "content");
    if (path == NULL || content == NULL) {
        free(path);
        free(content);
        return fmt("error: need string arguments 'path' and 'content'");
    }
    size_t len = strlen(content);
    int rc = write_file(path, content, len);
    free(path);
    free(content);
    if (rc != 0) {
        return fmt("error: cannot write file");
    }
    return fmt("ok (%d bytes written)", (int)len);
}

static char *tool_bash(const cJSON *args)
{
    char *command = arg_string(args, "command");
    if (command == NULL) {
        return fmt("error: missing string argument 'command'");
    }
    /* stderr wird mit eingesammelt; die subshell-klammer stellt
     * sicher, dass NUTZER-redirections (z.B. "echo x 1>&2")
     * zuerst wirken und "2>&1" sie nicht kaputtmacht */
    char cmd_buf[512];
    int n = snprintf(cmd_buf, sizeof cmd_buf, "(%s) 2>&1", command);
    free(command);
    if (n < 0 || (size_t)n >= sizeof cmd_buf) {
        return fmt("error: command too long");
    }

    /* die shell ist hier der zweck des tools, nicht der unfall */
    // NOLINTNEXTLINE(bugprone-command-processor)
    FILE *p = popen(cmd_buf, "r");
    if (p == NULL) {
        return fmt("error: popen failed");
    }

    /* output einsammeln, bis datei-ende oder tool-budget */
    size_t cap = 4096;
    size_t got = 0;
    char *buf = malloc(cap);
    if (buf == NULL) {
        pclose(p);
        return NULL;
    }
    for (;;) {
        if (got + 1 >= cap) {
            if (cap > TOOL_MAX_OUT) {
                break; /* genug: rest ignorieren */
            }
            char *grown = realloc(buf, cap * 2);
            if (grown == NULL) {
                free(buf);
                pclose(p);
                return NULL;
            }
            buf = grown;
            cap *= 2;
        }
        size_t chunk = fread(buf + got, 1, cap - got - 1, p);
        got += chunk;
        buf[got] = '\0';
        if (chunk == 0) {
            break;
        }
    }
    int status = pclose(p);
    int exit_code = -1;
    if (WIFEXITED(status)) {
        exit_code = WEXITSTATUS(status);
    }

    /* exit-code hinten dran: das model soll erkennen koennen, ob
     * das kommando erfolgreich war */
    char *out = realloc(buf, got + 32);
    if (out == NULL) {
        free(buf);
        return NULL;
    }
    (void)snprintf(out + got, 32, "\n[exit: %d]", exit_code);
    return cap_result(out);
}

/* ------------------------------------------------------------------ */
/* public api                                                          */
/* ------------------------------------------------------------------ */
char *tool_execute(const char *name, const char *arguments_json)
{
    if (name == NULL || arguments_json == NULL) {
        return fmt("error: tool name or arguments missing");
    }
    if (tool_find(name) == NULL) {
        return fmt("error: unknown tool '%s'", name);
    }

    cJSON *args = cJSON_Parse(arguments_json);
    if (args == NULL) {
        return fmt("error: arguments are not valid json");
    }

    char *result = NULL;
    if (strcmp(name, "read_file") == 0) {
        result = tool_read_file(args);
    } else if (strcmp(name, "write_file") == 0) {
        result = tool_write_file(args);
    } else if (strcmp(name, "bash") == 0) {
        result = tool_bash(args);
    } else {
        result = fmt("error: unknown tool '%s'", name);
    }
    cJSON_Delete(args);

    if (result == NULL) {
        return fmt("error: tool failed");
    }
    return result;
}