#ifndef MAX_AGENT_THEME_H
#define MAX_AGENT_THEME_H

#include <stdbool.h>
#include <stddef.h>

/* Ein Theme besteht aus SGR-sequenzen, die direkt in den frame-buffer
 * geschrieben werden. Palette-indizes (z.B. 36 = cyan) werden vom
 * terminal automatisch auf das dort konfigurierte farbschema gemappt -
 * wir erraten also keine RGB-werte, sondern folgen dem terminal. */
/* ------------------------------------------------------------------ */
/* Rollen im chat-verlauf. jede rolle ist eine SGR-sequenz – das darf */
/* eine palette-farbe sein (\x1b[31m) oder ein attribut (\x1b[1m      */
/* bold, \x1b[2m faint). welches von beidem, entscheidet das theme.  */
/* ------------------------------------------------------------------ */
typedef enum {
    THEME_ROLE_USER = 0,  /* label "you"                             */
    THEME_ROLE_ASSISTANT, /* label "max" – default: die match-farbe  */
    THEME_ROLE_ERROR,     /* label "err"                             */
    THEME_ROLE_TOOL,      /* label "tool" und die tool-call-zeilen   */
    THEME_ROLE_SYSTEM,    /* label "sys"                             */
    THEME_ROLE_NOTICE,    /* label "ctx": hinweise der app           */
    THEME_ROLE_DIM,       /* beiwerk: thinking, scroll-hinweise      */
    THEME_ROLE_COUNT,
} ThemeRole;

/* nach einer rollen-sequenz zu schreiben: raeumt attribute UND
 * vordergrund ab, damit es egal ist, wofuer sich das theme
 * entschieden hat. der hintergrund bleibt unberuehrt. */
#define THEME_ROLE_RESET "\x1b[22;39m"

typedef struct {
    const char *name;
    const char *match; /* vordergrund-farbe der gematchten zeichen */
    const char *reset; /* zurueck auf default-vordergrund */
    /* pro rolle eine SGR-sequenz; NULL = eingebauter default
     * (siehe theme_role). so muss ein theme nur abweichen, wo es
     * wirklich etwas anderes will. */
    const char *roles[THEME_ROLE_COUNT];
} Theme;

/* SGR-sequenz fuer eine rolle im aktuellen theme. nie NULL: setzt
 * das theme nichts eigenes, kommt der eingebaute default. */
const char *theme_role(ThemeRole role);

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
