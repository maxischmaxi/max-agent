#ifndef MAX_AGENT_THEME_H
#define MAX_AGENT_THEME_H

#include <stdbool.h>
#include <stddef.h>

/* Ein Theme besteht aus SGR-sequenzen, die direkt in den frame-buffer
 * geschrieben werden. Palette-indizes (z.B. 36 = cyan) werden vom
 * terminal automatisch auf das dort konfigurierte farbschema gemappt -
 * wir erraten also keine RGB-werte, sondern folgen dem terminal. */
typedef struct {
    const char *name;
    const char *match; /* vordergrund-farbe der gematchten zeichen */
    const char *reset; /* zurueck auf default-vordergrund */
} Theme;

/* Default-theme aus dem terminal ableiten: fragt per OSC 10/11 die
 * default-farben ab und waehlt danach einen passenden palette-index
 * (helles terminal -> dunkle farbe, dunkles terminal -> helle farbe).
 * Bytes, die keine antwort sind (z.B. fruehe tastatur-eingaben),
 * landen in leftover und muessen als eingabe erhalten bleiben. */
void theme_init(char *leftover, size_t cap, size_t *len);

/* Eigenes theme setzen / aktuelles abfragen (fuer spaeter). */
void theme_set(const Theme *t);
const Theme *theme_current(void);

/* ------------------------------------------------------------------ */
/* Benannte themes (settings-dialog). match bleibt ein palette-index */
/* – das terminal mappt ihn auf sein farbschema, genauso wie bei      */
/* "auto". die namen verweisen auf beliebte terminal-paletten: wer  */
/* die passende palette im terminal aktiviert hat, sieht das dazuge- */
/* hoerige highlight.                                                */
/* ------------------------------------------------------------------ */

/* theme nach name anwenden ("auto" = aus terminal-farben ableiten).
 * liefert false, wenn der name unbekannt ist. */
bool theme_select(const char *name);

/* auswahl-liste fuer den settings-dialog: index 0 ist immer "auto",
 * danach folgen die benannten themes. count >= 2. */
int theme_option_count(void);
const char *theme_option_name(int idx);

/* Eine OSC-10/11-antwort parsen ("11;rgb:1c1c/2b2b/1616").
 * rgb-kanaele koennen 1-4 hex-ziffern breit sein, skaliert auf 0-255.
 * Fuer tests oeffentlich. */
bool theme_parse_color_reply(const char *osc, unsigned rgb[3]);
int theme_names(const char *out[], int out_max);
int names_match(const char *const *names, int total, const char *search,
                int *out, int out_max);

#endif
