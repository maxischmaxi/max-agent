#ifndef MAX_AGENT_DEBUG
#define MAX_AGENT_DEBUG

#include <stddef.h>

/* ------------------------------------------------------------------ */
/* Debug-Sidebar: nur in Debug-Builds aktiv (release setzt -DNDEBUG).  */
/* Nimmt ~28% der rechten Seite ein, getrennt durch '|'.               */
/* ------------------------------------------------------------------ */

/* die defines + typedef muessen ausserhalb des NDEBUG-guards stehen:
 * draw() in main.c referenziert den typ in beiden builds. */
#define DBG_LINE_MAX  160
#define DBG_MAX_LINES 512

typedef struct {
    int head;  /* index des aeltesten eintrags */
    int count; /* Anzahl gueltiger eintraege */
    char log[DBG_MAX_LINES][DBG_LINE_MAX];
} DebugState;

#ifndef NDEBUG

/* draw_sidebar liest nur -> const-pointer */
size_t draw_sidebar(const DebugState *st, char *buf, size_t pos, int row,
                    int rows, int dw);

#if defined(__GNUC__) || defined(__clang__)
/* fmt ist parameter 2 -> attribute-indizes verschoben */
#define DBG_PRINTF_FMT __attribute__((format(printf, 2, 3)))
#else
#define DBG_PRINTF_FMT
#endif

void dbg_logf(DebugState *st, const char *fmt, ...) DBG_PRINTF_FMT;

/* das macro bekommt den state als erstes argument */
#define dbg_log(st, ...) dbg_logf(st, __VA_ARGS__)

#else

/* release: alle argumente verschwinden, call-sites bleiben gleich */
#define dbg_log(st, ...) ((void)0)

#endif /* NDEBUG */

/* steuerzeichen in s durch '.' ersetzen. die sidebar rendert text
 * und darf niemals escape-sequenzen (z.B. farb-codes) ausgeben.
 * in beiden builds verfuegbar (auch tests nutzen sie). */
void dbg_sanitize(char *s);

int dbg_width(int cols);

#endif
