#ifndef MAX_AGENT_STATE
#define MAX_AGENT_STATE

#include <signal.h>
#include <stdbool.h>

#include "chat.h"
#include "context.h"
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

    Chat chat;       /* transcript der unterhaltung (modul chat.c):
                      * zero-initialisiert, chat_free am app-ende */
    int chat_scroll; /* render-zeilen, die im viewport unten abgeschnitten */
                     /* sind (pgup); 0 = ans ende folgen. layout_compute */
                     /* klemmt und schreibt normalisiert zurueck */

    bool confirm_quit;  /* when ctrl+c was hit the first time */
    bool models_dialog; /* when entering the models dialog */
    bool cmd_active;    /* when typing "/", currently writing a command */
    bool settings_dialog;
    bool theme_sub; /* theme-untermenue offen (nur mit settings_dialog) */
    volatile sig_atomic_t resized;
    bool busy; /* anfrage laeuft: draw zeigt thinking-indikator, die */
               /* UI blockiert bis die antwort da ist               */

    /* token-buchhaltung ueber die runden hinweg: wieviel der
     * letzte request geschaetzt/wirklich gekostet hat und wieviel
     * verlauf dabei weggelassen wurde (context.c) */
    CtxUsage ctx;
    DialogState dialog;
    bool quit;
    bool dirty;
} AppState;

#endif
