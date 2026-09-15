#ifndef MAX_AGENT_STATE
#define MAX_AGENT_STATE

#include <signal.h>
#include <stdbool.h>

#include "input.h"

/* ------------------------------------------------------------------ */
/* DialogState: suchtext + cursor eines offenen dialogs. gehoert zur   */
/* App und ueberlebt redraws/resizes; wird beim dialog-ende geleert.  */
/* ------------------------------------------------------------------ */
typedef struct {
    char search[64]; /* aktueller suchtext */
    int selected;    /* cursor in der gefilterten liste, 0-basiert */
    int scroll;      /* erster sichtbarer eintrag */
} DialogState;

/* ------------------------------------------------------------------ */
/* AppState: DER zentrale zustand der app. die chat-eingabe ist als   */
/* Input-member eingebettet – alles darueber (draw, handle_*, layout) */
/* reicht genau einen State-Pointer durch. die input-modulfunktionen  */
/* (input_char, input_reset, ...) bekommen &state.input.              */
/* ------------------------------------------------------------------ */
typedef struct {
    Input input; /* chat-eingabe: zeilen + cursor (modul input.c) */

    bool confirm_quit;  /* when ctrl+c was hit the first time */
    bool models_dialog; /* when entering the models dialog */
    bool cmd_active;    /* when typing "/", currently writing a command */
    bool settings_dialog;
    bool theme_sub; /* theme-untermenue offen (nur mit settings_dialog) */
    volatile sig_atomic_t resized;
    DialogState dialog;
    bool quit;
    bool dirty;
} AppState;

#endif
