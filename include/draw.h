#ifndef MAX_AGENT_DRAW
#define MAX_AGENT_DRAW

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

    /* befehlsliste direkt unter dem eingabefeld */
    int cmd_top;
    int cmd_h;

    int quit_row; /* 0 = keine quit-meldung anzeigen */

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
} Layout;

typedef enum {
    SLOT_BLANK,      /* leere zeile */
    SLOT_BORDER,     /* trenn-linie aus em-dashes */
    SLOT_INPUT,      /* zeile des eingabefeldes */
    SLOT_CMD_EMPTY,  /* hinweis: kein befehl passt */
    SLOT_CMD,        /* treffer der befehlsliste */
    SLOT_DLG_BORDER, /* trenn-linie ueber dem dialog */
    SLOT_DLG_SEARCH, /* such-zeile mit cursor */
    SLOT_DLG_EMPTY,  /* hinweis: kein eintrag passt */
    SLOT_MODEL,      /* eintrag der modell-liste */
    SLOT_SETTING,    /* eintrag der settings-liste */
    SLOT_THEME,      /* eintrag der theme-auswahl */
    SLOT_QUIT,       /* quit-bestaetigung */
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
Slot layout_slot(const Layout *lt, int row);
void draw_slot(Frame *f, const Layout *lt, const Slot *s, const AppState *st,
               const Config *cfg);
void draw(int rows, int cols, AppState *state, const DebugState *dbg,
          const Config *cfg);

#endif
