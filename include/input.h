#ifndef MAX_AGENT_INPUT
#define MAX_AGENT_INPUT

#include <stdbool.h>
#include <stddef.h>
#define INPUT_MAX_LINES 16

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
void input_char(Input *in, char c, int cols);
void input_free(Input *in);
int bottom_border_for(int rows, int list_h, bool g_confirm_quit);

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
