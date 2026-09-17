#ifndef MAX_AGENT_KEYS
#define MAX_AGENT_KEYS

#include <stdbool.h>
#include <sys/types.h>

#include "config.h"
#include "state.h"

/* lesepuffer fuer tastatur-bytes. gross genug, dass eine
 * tastenwiederholung oder ein kurzer paste komplett in einen read
 * passt (eine CSI-sequenz ist bis zu 9 byte lang) */
#define SEQ_MAX 128

typedef enum {
    KEY_NONE = 0,
    KEY_CHAR,
    KEY_ENTER,
    KEY_NEWLINE,
    KEY_CTRL_C,
    KEY_CTRL_Q,
    KEY_BACKSPACE,
    KEY_ESCAPE,
    KEY_UP,
    KEY_DOWN,
    KEY_PGUP, /* chat-verlauf zurueckblaettern (CSI 5~) */
    KEY_PGDN, /* chat-verlauf vorblaettern, bis unten (CSI 6~) */
    KEY_TAB,  /* tab: chat-modus = befehls-vervollstaendigung */

    /* POSIX/readline-shortcuts (nur im normalen chat-input aktiv,
     * dialoge ignorieren sie). die semantik entspricht bash/readline:
     *   ctrl+a anfang der zeile        ctrl+e ende der zeile
     *   ctrl+b ein zeichen zurueck     ctrl+f ein zeichen vor
     *   ctrl+w letztes wort loeschen   ctrl+u ganze zeile loeschen
     *   ctrl+k bis zeilenende loeschen ctrl+d zeichen unter cursor
     *   ctrl+h zeichen hinter cursor   ctrl+l bildschirm neu
     *   ctrl+p history zurueck (spaeter)  ctrl+n history vor (spaeter) */
    KEY_CTRL_A,
    KEY_CTRL_B,
    KEY_CTRL_D,
    KEY_CTRL_E,
    KEY_CTRL_F,
    KEY_CTRL_H,
    KEY_CTRL_K,
    KEY_CTRL_L,
    KEY_CTRL_N,
    KEY_CTRL_P,
    KEY_CTRL_T, /* zeichen vor/mit cursor tauschen */
    KEY_CTRL_U,
    KEY_CTRL_W,

    /* Meta-bindings (M-x, meist alt+x) – ebenfalls readline-emacs-
     * modus. unterschied zu ctrl+w: hier ist ein "wort" eine
     * alphanumerische sequenz (satzzeichen sind grenzen), ctrl+w
     * nimmt whitespace als grenze:
     *   alt+b wort zurueck             alt+f wort vor
     *   alt+d wort vorwaerts killen   alt+backspace wort zurueck killen
     *   alt+t woerter tauschen        ctrl+t zeichen tauschen
     *   alt+u grossschreiben          alt+l kleinschreiben
     *   alt+c wort kapitalisieren */
    KEY_ALT_B,
    KEY_ALT_F,
    KEY_ALT_D,
    KEY_ALT_BACKSPACE,
    KEY_ALT_T,
    KEY_ALT_U,
    KEY_ALT_L,
    KEY_ALT_C,
} KeyKind;

typedef struct {
    KeyKind kind;
    /* KEY_CHAR: das getippte zeichen als utf-8-sequence (1..4
     * bytes, immer '\0'-terminiert) – umlaute und sz sind mehr-
     * byte-folgen. alles, was kein KEY_CHAR ist, laesst es leer */
    char ch[5];
} Key;

typedef struct {
    bool dirty;
    bool quit;
} HandleResponse;

Key key_from_escape(const char *seq, ssize_t len);
Key key_from_byte(char c);

/* rohe utf-8-folge (die komplette sequenz, 1..4 bytes) als
 * KEY_CHAR: so kommen umlaute und sz im legacy-encoding an. */
Key key_from_utf8(const char *buf, size_t len);

Key key_read(void);

/* laenge der ersten vollstaendigen tastensequenz in buf. 0 heisst
 * unvollstaendig: auf mehr bytes warten. ein read kann mehrere
 * tasten enthalten, deshalb wird byte-genau abgeschnitten. */
size_t key_seq_len(const char *buf, size_t len);

/* bytes vor die tastatur-eingabe einreihen (z.B. fruehe eingaben,
 * die waehrend des terminal-query-fensters angekommen sind).
 * der interne puffer laeuft nie ueber: ueberschuss wird verworfen. */
void keys_unread(const char *buf, size_t len);

/* waehrend einer laufenden anfrage: hat der benutzer abgebrochen?
 * prueft den tasten-puffer und stdin, ohne je zu blockieren.
 *
 * abbruch sind ctrl+c und ein ALLEIN stehendes escape – pfeil-
 * tasten und andere sequenzen fangen auch mit 0x1b an und duerfen
 * nicht stoppen. alles andere bleibt erhalten und
 * landet nach der anfrage im eingabefeld – wer waehrend der antwort
 * weitertippt, verliert nichts. beim abbruch wird der rest der
 * eingabe dagegen verworfen: wer stoppt, will nicht gleichzeitig
 * tippen. */
bool keys_abort_pressed(void);
void handle_key(AppState *state, Config *cfg, int *rows, int *cols);

#endif
