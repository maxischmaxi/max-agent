#ifndef MAX_AGENT_DRAW
#define MAX_AGENT_DRAW

#include "chat.h"
#include "config.h"
#include "state.h"
#include "utils.h"

typedef struct {
    UIMode mode;
    int rows;
    int main_w;

    /* eingabefeld inkl. rahmen: erste/letzte zeile, 1-basiert */
    int input_top;
    int input_bottom;

    /* chat-verlauf (MODE_INPUT): zeilen ueber der eingabe-box.
     * chat_lines zeigt in eine modul-statische arena von draw.c
     * und ist nur bis zum naechsten layout_compute gueltig. */
    ChatLine *chat_lines;
    size_t chat_lines_len;
    int chat_h;     /* zeilen, die dem verlauf insgesamt gehoeren */
    int chat_top;   /* erste zeile mit verlaufs-inhalt (1 oder 2:  */
                    /* der "weiter oben"-hinweis belegt zeile 1)   */
    int chat_first; /* index der ersten sichtbaren zeile */
    int busy_row;   /* zeile des thinking-indikators, 0 = keiner */

    /* scroll-hinweise: wieviele render-zeilen ausserhalb des
     * viewports liegen, und in welcher zeile der hinweis steht
     * (0 = kein hinweis). sie kosten je eine viewport-zeile und
     * erscheinen nur, wenn dafuer platz bleibt. */
    int more_above;
    int more_below;
    int more_above_row;
    int more_below_row;

    /* soft-wrap des eingabefelds: nutzbare textbreite und die erste
     * sichtbare bildschirmzeile (gescrollt wird nur, wenn der text
     * hoeher ist als die box werden darf) */
    int input_w;
    size_t input_first;

    /* befehlsliste direkt unter dem eingabefeld */
    int cmd_top;
    int cmd_h;
    /* statt der befehlsliste steht dort der hinweis, dass das
     * eingabefeld gerade den system-prompt bearbeitet */
    bool prompt_hint;

    int quit_row;   /* 0 = keine quit-meldung anzeigen */
    int status_row; /* erste der beiden statuszeilen */

    /* dialog-box (MODE_MODELS / MODE_SETTINGS / MODE_THEME),
     * box waechst von unten: */
    int box_top;    /* border-zeile */
    int box_bottom; /* letzte eintrags-zeile = rows - 1 */
    int entries_h;  /* sichtbare eintraege, max rows/2 */

    /* daten fuer die renderer */
    char prefix[64];
    char search[64];
    /* treffer-indizes aller dialoge. WICHTIG: auf DIALOG_MATCH_MAX
     * dimensioniert – COMMAND_COUNT wuerde ab dem 5. modell
     * ueberlaufen (models_match schreibt bis zu 128 indizes) */
    int match_idx[DIALOG_MATCH_MAX];
    int match_count;
    int scroll;   /* erster sichtbarer treffer */
    int selected; /* flacher index des angewaehlten eintrags */
    int id_col;   /* breite der namens-spalte (alignment, wie cmd_name_col) */
    int name_col; /* dito fuer die session-liste: name/preview-spalte */
} Layout;

typedef enum {
    SLOT_BLANK,         /* leere zeile */
    SLOT_BORDER,        /* trenn-linie aus em-dashes */
    SLOT_INPUT,         /* zeile des eingabefeldes */
    SLOT_CMD_EMPTY,     /* hinweis: kein befehl passt */
    SLOT_CMD,           /* treffer der befehlsliste */
    SLOT_MSG_USER,      /* chat-verlauf: benutzer-zeile */
    SLOT_MSG_ASSISTANT, /* chat-verlauf: modell-zeile */
    SLOT_MSG_ERROR,     /* chat-verlauf: fehler-meldung */
    SLOT_MSG_TOOL,      /* chat-verlauf: tool-ergebnis */
    SLOT_MSG_SYSTEM,    /* chat-verlauf: system-meldung */
    SLOT_MORE_ABOVE,    /* hinweis: verlauf geht oberhalb weiter */
    SLOT_MORE_BELOW,    /* hinweis: verlauf geht unterhalb weiter */
    SLOT_BUSY,          /* thinking-indikator waehrend einer anfrage */
    SLOT_DLG_BORDER,    /* trenn-linie ueber dem dialog */
    SLOT_DLG_SEARCH,    /* such-zeile mit cursor */
    SLOT_DLG_EMPTY,     /* hinweis: kein eintrag passt */
    SLOT_MODEL,         /* eintrag der modell-liste */
    SLOT_SETTING,       /* eintrag der settings-liste */
    SLOT_THEME,         /* eintrag der theme-auswahl */
    SLOT_PROMPT_OPT,    /* eintrag der system-prompt-auswahl */
    SLOT_PROMPT_HINT,   /* hinweis: eingabefeld bearbeitet den prompt */
    SLOT_SESSION,       /* eintrag der session-liste (resume-dialog) */
    SLOT_QUIT,          /* quit-bestaetigung */
    SLOT_STATUS_MODEL,  /* statuszeile 1: modell + kontextfenster */
    SLOT_STATUS_TOKENS, /* statuszeile 2: verbrauch (+ spaeter session) */
} SlotKind;

typedef struct {
    SlotKind kind;
    int index; /* listen-index: eingabe-zeile / befehl / modell */
} Slot;

typedef struct {
    char *buf;
    size_t pos;
} Frame;

typedef struct {
    Frame *f;
    int width; /* zellen bis zum rand des hauptbereichs */
    int cells; /* bereits geschriebene sichtbare zellen */
} Row;

void layout_dialog_box(Layout *lt, int rows, DialogState *d);

void layout_compute(Layout *lt, int rows, int cols, UIMode mode, AppState *st,
                    const Config *cfg);

/* nutzbare textbreite einer eingabezeile bei dieser terminalbreite:
 * ohne debug-sidebar, ohne das " > "-praefix und ohne die spalte,
 * in der der cursor-block am zeilenende sitzt. keys.c braucht
 * dieselbe zahl wie das layout, damit tippen und zeichnen
 * denselben umbruch sehen. */
int input_field_width(int cols);

/* die beiden statuszeilen am unteren rand sind fest reserviert:
 * sie zeigen immer, mit welchem modell gesprochen wird und was die
 * sitzung bisher gekostet hat. eingabefeld und befehlsliste liegen
 * darueber und rechnen deshalb mit entsprechend weniger zeilen. */
#define STATUS_H 2
Slot layout_slot(const Layout *lt, int row);
void draw_slot(Frame *f, const Layout *lt, const Slot *s, const AppState *st,
               const Config *cfg);
void draw(int rows, int cols, AppState *state, const Config *cfg);

#endif
