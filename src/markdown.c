/* markdown.c: zeilenlokales highlight-scanning der ki-antworten.
 * siehe markdown.h fuer die design-gruende (streaming-sicherheit
 * durch zeilenanfang-muster ohne block-zustand). */

#include "markdown.h"

#include <string.h>

/* ziffernfolge am anfang, gefolgt von '.' oder ')': nummerierte
 * liste ("1.", "42." ...). rueckgabe = laenge des markers, 0 = nein */
static size_t scan_num_marker(const char *s, size_t len)
{
    size_t i = 0;
    while (i < len && s[i] >= '0' && s[i] <= '9') {
        i++;
    }
    if (i == 0 || i >= len) {
        return 0; /* keine ziffer bzw. nichts danach */
    }
    if (s[i] != '.' && s[i] != ')') {
        return 0;
    }
    return i + 1; /* ziffern + trenner */
}

void md_scan(const char *text, size_t off, size_t len, bool line_start,
             MdLine *out)
{
    out->kind = MD_NONE;
    out->level = 0;
    out->marker = off;
    out->marker_len = 0;

    if (!line_start || len == 0) {
        return; /* umbruch-folgezeile: nur die erste zeile zaehlt */
    }

    const char *s = text + off;

    /* fuehrende leerzeichen sind erlaubt (bis zu 3, wie commonmark),
     * ein tieferer einzug gehoert zum text */
    size_t i = 0;
    while (i < len && i < 3 && s[i] == ' ') {
        i++;
    }
    if (i >= len) {
        return; /* nur leerzeichen */
    }

    /* ueberschrift: #+ gefolgt von space oder zeilenende. "#abc"
     * (ohne space) ist KEINE ueberschrift, auch nicht in commonmark.
     * beim streaming kann das zeilenende noch fehlen: "#hal" ist
     * dann text – erst das space macht es zur headline. das ist
     * konservativ und aendert sich nicht mehr, wenn der rest kommt */
    if (s[i] == '#') {
        size_t h = i;
        size_t n = 0;
        while (h < len && s[h] == '#') {
            h++;
            n++;
        }
        if (h < len && s[h] == ' ') {
            out->kind = MD_HEAD;
            out->level = (int)n;
            out->marker = off + i;
            out->marker_len = h - i; /* die #-folge selbst */
            return;
        }
        return; /* '#...' ohne space: normaler text */
    }

    /* liste: -, *, + gefolgt von space */
    if ((s[i] == '-' || s[i] == '*' || s[i] == '+') && i + 1 < len &&
        s[i + 1] == ' ') {
        out->kind = MD_LIST;
        out->marker = off + i;
        out->marker_len = 1; /* nur das punkt-zeichen, space bleibt text */
        return;
    }

    /* nummerierte liste: "1.", "42." */
    size_t num = scan_num_marker(s + i, len - i);
    if (num > 0) {
        out->kind = MD_LIST;
        out->marker = off + i;
        out->marker_len = num;
        return;
    }

    /* zitat: > gefolgt von space oder zeilenende */
    if (s[i] == '>') {
        out->kind = MD_QUOTE;
        out->marker = off + i;
        out->marker_len = 1;
        return;
    }
}