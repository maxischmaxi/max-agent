#ifndef MAX_AGENT_INPUT
#define MAX_AGENT_INPUT

#include <stdbool.h>
#include <stddef.h>
#define INPUT_MAX_LINES 16

/* obergrenze je LOGISCHER zeile. frueher war das die feldbreite –
 * seit dem soft-wrap bricht eine lange zeile einfach um, also
 * begrenzt hier nur noch der gesunde menschenverstand (16 zeilen
 * dieser laenge sind ein sehr langer system-prompt). */
#define INPUT_MAX_LINE_BYTES 4096

typedef struct {
    char *lines[INPUT_MAX_LINES];
    size_t count;
    /* cursor: byte-position in lines[cursor_line], an der das
     * naechste zeichen landet. die invariante gilt zeilenbezogen:
     * cursor_line < count und cursor <= strlen(lines[cursor_line]).
     * alle input_*()-funktionen bewahren sie. */
    size_t cursor_line;
    size_t cursor;
} Input;

const char *last_word(const Input *in);
bool input_in_cmd(const Input *in);
void cmd_prefix(const Input *in, char *out, size_t out_sz);
void input_init(Input *in);
void input_reset(Input *in);

/* die eingabe komplett durch text ersetzen: an '\n' in zeilen
 * zerlegt, cursor ans ende der letzten zeile. das ist der gegenpart
 * zu chat_flatten_input() und holt history-eintraege zurueck ins
 * feld. mehr als INPUT_MAX_LINES zeilen passen nicht hinein – der
 * rest faellt weg. NULL/"" leeren das feld. */
void input_set_text(Input *in, const char *text);
void input_backspace(Input *in);
void input_newline(Input *in, int rows, int list_h, bool g_confirm_quit);
/* ein zeichen an der cursor-position einfuegen. die feldbreite
 * spielt keine rolle mehr: zu langer text bricht beim zeichnen um
 * (siehe soft-wrap unten), statt verschluckt zu werden. */
void input_char(Input *in, char c);
void input_free(Input *in);
int bottom_border_for(int rows, int list_h, bool g_confirm_quit);

/* ------------------------------------------------------------------ */
/* Soft-wrap: lines[] sind die LOGISCHEN zeilen (durch shift+enter    */
/* getrennt). laeuft eine davon ueber die feldbreite, wird sie beim   */
/* ZEICHNEN auf mehrere bildschirmzeilen verteilt – der text selbst  */
/* bleibt unveraendert.                                               */
/*                                                                    */
/* das ist der grund fuer die trennung: wuerde der umbruch echte      */
/* zeilen einfuegen, waere nach einem resize nicht mehr zu erkennen,  */
/* welche zeilenumbrueche der benutzer wollte und welche vom umbruch  */
/* stammen – der abgeschickte text haenge dann an der fenstergroesse. */
/*                                                                    */
/* umgebrochen wird ZEICHENWEISE an der feldkante, nicht am wort      */
/* (anders als im chat-verlauf): beim tippen soll text dort bleiben,  */
/* wo er steht, und die cursor-position eindeutig sein. so machen es  */
/* auch shells. utf-8-codepoints werden nie zerschnitten.             */
/* ------------------------------------------------------------------ */

/* wieviele bildschirmzeilen die eingabe bei dieser breite belegt.
 * jede logische zeile ergibt mindestens eine – auch die leere. */
size_t input_screen_rows(const Input *in, int width);

/* den abschnitt der idx-ten bildschirmzeile bestimmen: logische
 * zeile, byte-offset darin, byte-laenge. false = idx liegt hinter
 * der letzten zeile. out-parameter duerfen NULL sein. */
bool input_screen_row(const Input *in, int width, size_t idx, size_t *line,
                      size_t *off, size_t *len);

/* bildschirmzeile und spalte (in zellen) des cursors */
void input_cursor_screen(const Input *in, int width, size_t *row, size_t *col);

/* cursor eine bildschirmzeile hoch/runter, die spalte moeglichst
 * halten. false = es gibt in der richtung keine zeile mehr (dann
 * greift in keys.c die history). */
bool input_screen_up(Input *in, int width);
bool input_screen_down(Input *in, int width);

/* ------------------------------------------------------------------ */
/* Cursor-utility: alles, was man zum steuern braucht. die bewegungs-  */
/* funktionen aendern nie text; die kill-funktionen schon – alle sind  */
/* multiline-faehig: bewegung laeuft ueber zeilengrenzen, backspace/  */
/* delete/kill loeschen auch zeilenumbrueche, newline splittet an der */
/* cursor-position (POSIX/readline-semantik wie im terminal).        */
/* ------------------------------------------------------------------ */

/* laenge der aktuellen cursor-zeile */
size_t input_len(const Input *in);

/* cursor in der aktuellen zeile setzen, auf 0..len geklemmt */
void input_cursor_set(Input *in, size_t pos);

/* cursor-zeile wechseln; der byte-cursor klemmt dann auf die laenge
 * der neuen zeile */
void input_cursor_line_set(Input *in, size_t line);

/* an anfang / ans ende der aktuellen zeile springen */
void input_cursor_home(Input *in);
void input_cursor_end(Input *in);

/* ein zeichen zurueck/vor; am zeilenrand ueber die zeilengrenze in
 * die nachbarzeile. false = anfang/ende des gesamten buffers */
bool input_cursor_left(Input *in);
bool input_cursor_right(Input *in);

/* zeilenumbruch VOR dem cursor loeschen: aktuelle zeile haengt an
 * die vorherige. false = keine vorherige zeile (ctrl+w am anfang) */
bool input_join_prev(Input *in);

/* zeilenumbruch HINTER dem cursor loeschen: naechste zeile haengt an
 * die aktuelle. false = keine naechste zeile */
bool input_join_next(Input *in);

/* zeichen UNTER dem cursor loeschen; am zeilenende den umbruch dahinter
 * (ctrl+d) */
bool input_delete_forward(Input *in);

/* von cursor bis zeilenende loeschen; am ende den umbruch dahinter
 * (ctrl+k) */
bool input_kill_to_end(Input *in);

/* von zeilenanfang bis cursor loeschen, cursor auf 0 (ctrl+u, bash:
 * unix-line-discard) */
bool input_kill_line(Input *in);

/* letztes wort vor dem cursor loeschen (inkl. whitespace, ctrl+w);
 * am zeilenanfang den umbruch davor */
bool input_kill_last_word(Input *in);

/* ------------------------------------------------------------------ */
/* Meta-bindings (readline "wort" = alphanumerische sequenz, satz-  */
/* zeichen sind grenzen – anders als ctrl+w mit whitespace-grenzen). */
/* wortbewegung laeuft ueber zeilengrenzen.                          */
/* ------------------------------------------------------------------ */

/* an den anfang des vorherigen worts (alt+b) */
bool input_word_left(Input *in);

/* an das ende des naechsten worts (alt+f) */
bool input_word_right(Input *in);

/* wort ab/naech dem cursor vorwaerts killen; am zeilenende den
 * umbruch dahinter (alt+d) */
bool input_kill_word(Input *in);

/* wort vor dem cursor rueckwaerts killen (alnum-grenzen, anders als
 * ctrl+w); am zeilenanfang den umbruch davor (alt+backspace) */
bool input_kill_word_back(Input *in);

/* zeichen vor/mit cursor vertauschen (ctrl+t). am zeilenende werden
 * die letzten zwei getauscht, am anfang passiert nichts */
bool input_transpose_chars(Input *in);

/* wort vor dem cursor mit dem wort danach vertauschen (alt+t) */
bool input_transpose_words(Input *in);

/* aktuelles/folgendes wort GROSS/klein/Kapitalisieren, cursor landet
 * am ende des worts (alt+u / alt+l / alt+c) */
bool input_word_upcase(Input *in);
bool input_word_downcase(Input *in);
bool input_word_capitalize(Input *in);

#endif
