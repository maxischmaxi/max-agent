#define _GNU_SOURCE // NOLINT(bugprone-reserved-identifier)

#include "debug.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "session.h"

/* ------------------------------------------------------------------ */
/* asan/ubsan-berichte (use-after-free, leaks, ...) in die datei       */
/* umleiten – nur in einem sanitize-build vorhanden, deshalb weak.    */
/* ------------------------------------------------------------------ */
/* NOLINTNEXTLINE(bugprone-reserved-identifier) */
__attribute__((weak)) void __sanitizer_set_report_path(const char *path);

static FILE *g_log = NULL;
static char g_path[512];

void dbg_init(const char *path)
{
    if (path == NULL) {
        return;
    }
    g_log = fopen(path, "w");
    if (g_log == NULL) {
        return; /* debug ist best-effort, nie fatal */
    }
    (void)setvbuf(g_log, NULL, _IOLBF, 8192);
    (void)snprintf(g_path, sizeof g_path, "%s", path);
    if (__sanitizer_set_report_path != NULL) {
        /* asan-berichte landen dann in <path>.PID.N statt auf stderr */
        __sanitizer_set_report_path(path);
    }
    dbg("debug-log: %s (pid %ld)", path, (long)getpid());
}

void dbg_rename(const char *session_id)
{
    if (g_log == NULL || session_id == NULL || g_path[0] == '\0') {
        return;
    }
    char to[64];
    (void)snprintf(to, sizeof to, "/tmp/max-agent-%s.log", session_id);
    if (rename(g_path, to) == 0) {
        (void)snprintf(g_path, sizeof g_path, "%s", to);
        dbg("debug-log umbenannt: %s", to);
    }
}

bool dbg_active(void)
{
    return g_log != NULL;
}

/* aktueller name der log-datei ("" ohne --debug): nach dem umbenennen
 * auf die session-id zeigt die statuszeile sofort den neuen namen */
const char *dbg_path(void)
{
    if (g_log == NULL) {
        return "";
    }
    return g_path;
}

void dbg(const char *fmt, ...)
{
    if (g_log == NULL) {
        return; /* fast-path: ohne --debug kostet debug nichts */
    }
    flockfile(g_log);

    struct timespec wall;
    struct timespec mono;
    (void)clock_gettime(CLOCK_REALTIME, &wall);
    (void)clock_gettime(CLOCK_MONOTONIC, &mono);
    long tid = syscall(SYS_gettid);
    (void)fprintf(g_log, "%ld.%03ld %ld.%03ld t%-6ld ", wall.tv_sec,
                  wall.tv_nsec / 1000000, mono.tv_sec, mono.tv_nsec / 1000000,
                  tid);
    va_list ap;
    va_start(ap, fmt);
    (void)vfprintf(g_log, fmt, ap);
    va_end(ap);
    (void)fputc('\n', g_log);

    funlockfile(g_log);
}