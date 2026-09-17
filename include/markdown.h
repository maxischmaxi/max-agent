#ifndef MAX_AGENT_MARKDOWN
#define MAX_AGENT_MARKDOWN

#include <stdbool.h>
#include <stddef.h>

/* ------------------------------------------------------------------ */
/* rudimentaeres markdown-highlighting fuer die ki-antworten.         */
/*                                                                    */
/* die regeln bewerten NUR den zeilenanfang – kein zustand ueber      */
/* zeilen hinweg, keine block-marker (``` fences). das ist bewusst:   */
/* waehrend des streamings besteht der text stueckchenweise, und ein */
/* scanner mit block-zustand wuerde beim jeden chunk anders interpre- */
/* tieren (fence offen/zu). zeilenanfang-muster sind dagegen schon   */
/* in der halben zeile eindeutig: "#" bleibt "#", ob der rest der     */
/* zeile noch kommt oder nicht. HEAD/MARKER sind damit von natur aus  */
/* streaming-sicher.                                                  */
/*                                                                    */
/* was erkannt wird (zeichen, gefolgt von space):
 *   Raute(n)            ueberschrift: fett + akzentfarbe
 *   bindestrich, stern, plus   listen-punkt: marker in akzentfarbe
 *   ziffer(n) mit punkt oder klammer   nummerierte liste
 *   groesser-als        zitat: marker in akzentfarbe               */
/* ------------------------------------------------------------------ */

typedef enum {
    MD_NONE = 0, /* gewoehnliche textzeile */
    MD_HEAD,     /* ueberschrift: # ## ### ... */
    MD_LIST,     /* aufzaehlung: - * + 1. 42. */
    MD_QUOTE,    /* zitat: > */
} MdKind;

typedef struct {
    MdKind kind;   /* was diese zeile ist                         */
    int level;     /* MD_HEAD: anzahl der '#' (1..)               */
    size_t marker; /* byte-offset des ersten marker-zeichens (>=off) */
    size_t marker_len; /* byte-laenge des markers ("- ", "1.", "### ") */
} MdLine;

/* eine render-zeile (byte-scheibe [off, off+len) aus dem nachrichten-
 * text) bewerten. line_start = die scheibe beginnt am originalen
 * zeilenanfang (chat_wrap liefert ln->first; bei umbruch-folgezei-
 * len ist line_start false und die pruefung ist immer MD_NONE).
 * streaming-sicher: unvollstaendige zeilen werden wie vollstaendige
 * bewertet, ein "# hal" ist eine gueltige ueberschrift, auch wenn
 * der rest noch kommt. */
void md_scan(const char *text, size_t off, size_t len, bool line_start,
             MdLine *out);

#endif