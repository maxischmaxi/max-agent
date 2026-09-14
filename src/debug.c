#include "debug.h"

#include <stdarg.h>
#include <stdio.h>

int dbg_width(int cols)
{
#ifdef NDEBUG
    (void)cols;
    return 0;
#else
    int w = (cols * 28) / 100; /* ~28% vom rechten rand */
    if (w < 12) {
        w = 12;
    }
    if (w > cols / 2) {
        w = cols / 2; /* hauptbereich nicht zerquetschen */
    }
    return w;
#endif
}

void dbg_sanitize(char *s)
{
    for (char *p = s; *p != '\0'; p++) {
        unsigned char u = (unsigned char)*p;
        if (u < 32 || u == 0x7f) {
            *p = '.';
        }
    }
}

#ifndef NDEBUG

void dbg_logf(DebugState *st, const char *fmt, ...)
{
    char line[DBG_LINE_MAX];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);

    /* rohe escape-sequenzen waeren sichtbare farb-codes im frame */
    dbg_sanitize(line);

    int idx;
    if (st->count < DBG_MAX_LINES) {
        idx = (st->head + st->count) % DBG_MAX_LINES;
        st->count++;
    } else {
        /* ring voll: aeltesten ueberschreiben */
        idx = st->head;
        st->head = (st->head + 1) % DBG_MAX_LINES;
    }
    snprintf(st->log[idx], DBG_LINE_MAX, "%s", line);
}

#endif /* NDEBUG */

#ifndef NDEBUG
/* eine sidebar-zeile zeichnen: '|' + log-text (unten verankert). */
size_t draw_sidebar(const DebugState *st, char *buf, size_t pos, int row,
                    int rows, int dw)
{
    size_t start = pos;

    buf[pos++] = '|';

    /* neueste nachricht immer ganz unten: idx relativ zum unteren rand */
    int shown = (st->count < rows) ? st->count : rows;
    int idx = row - (rows - shown) - 1;
    const char *text = (idx >= 0 && idx < shown)
                           ? st->log[(st->head + idx) % DBG_MAX_LINES]
                           : NULL;

    int n = 0;
    if (text) {
        while (text[n] != '\0' && n < dw) {
            buf[pos++] = text[n];
            n++;
        }
    }
    while (n < dw) {
        buf[pos++] = ' ';
        n++;
    }
    return pos - start;
}
#endif /* NDEBUG */
