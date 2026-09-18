#ifndef MAX_AGENT_STATE
#define MAX_AGENT_STATE

#include <signal.h>
#include <stdbool.h>

#include "chat.h"
#include "context.h"
#include "history.h"
#include "input.h"
#include "session.h"

/* busy-queue (keys.c): enter waehrend die ki arbeitet puffert die
 * nachricht hier. nach dem turn wird sie als eigener turn geschickt
 * ("bei naechster gelegenheit"); pfeil-hoch im leeren feld holt die
 * letzte zurueck ins eingabefeld. */
#define QUEUE_MAX 16

/* davon sichtbare eintraege im dock (die aeltesten werden
 * weggeklemmt, der hint zaehlt die gesamtzahl mit) */
#define QUEUE_VIS_MAX 3

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

    History history; /* zuletzt abgeschickte eingaben (modul
                      * history.c): pfeil-hoch holt sie zurueck */

    Chat chat; /* transcript der unterhaltung (modul chat.c):
                * zero-initialisiert, chat_free am app-ende. das
                * RENDERN ist inkrementell (draw.c): fertige
                * nachrichten werden einmal gedruckt und scrollen
                * ins terminal-scrollback, nur die aktuelle
                * streaming-zeile und der dock unten leben */

    bool confirm_quit;  /* when ctrl+c was hit the first time */
    bool models_dialog; /* when entering the models dialog */
    bool cmd_active;    /* when typing "/", currently writing a command */
    bool settings_dialog;
    bool sessions_dialog; /* resume-dialog: session-liste im dock */
    bool theme_sub;       /* theme-untermenue offen (nur mit settings_dialog) */
    volatile sig_atomic_t resized;
    bool busy; /* anfrage laeuft: die obere input-rahmenzeile zeigt */
               /* spinner + sekunden, die UI blockiert bis die ant-   */
               /* wort da ist                                          */
    long long busy_start_ms; /* mono-ms des turn-beginns; 0 = kein     */
                             /* laufender turn (spinner/sekunden)      */
    bool tool_running;       /* die ki arbeitet an einem tool-call: die */
                             /* live-zeile im chat zeigt einen spinner */
    long long worked_ms;     /* kumulierte zeit, die die ki in dieser  */
                             /* session gearbeitet hat (alle turns:   */
                             /* thinking, tool calls, antworten)      */

    /* token-buchhaltung ueber die runden hinweg: wieviel der
     * letzte request geschaetzt/wirklich gekostet hat und wieviel
     * verlauf dabei weggelassen wurde (context.c) */
    CtxUsage ctx;
    DialogState dialog;

    /* die session, in die gerade aufgezeichnet wird (session.c):
     * active = false, solange noch keine nachricht lief. beim
     * app-ende gehoert session_free dazu. */
    Session session;
    /* session-liste des resume-dialogs: wird beim oeffnen geladen
     * und beim schliessen (und am app-ende) freigegeben. ausser-
     * halb des dialogs ist sie leer. */
    SessionList sessions;
    bool quit;
    bool dirty;

    /* gebufferte nachrichten (FIFO): queue[0] ist die aelteste.
     * gefuellt wird nur waehrend busy (keys_drain), geleert nach dem
     * turn (flush_queue) oder per pfeil-hoch (letzte zurueck ins feld).
     * keys_queue_clear() gibt alles frei. */
    char *queue[QUEUE_MAX];
    size_t queue_n;

    /* warnzeile ueber dem eingabefeld, nur waehrend busy: 0 = keine,
     * 1 = befehl geht waehrend der antwort nicht, 2 = queue voll */
    int cmd_warn;
} AppState;

#endif
