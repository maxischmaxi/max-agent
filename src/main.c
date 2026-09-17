/* sigaction + nanosleep brauchen POSIX; ohne das feature-test-macro
 * deklariert c17 sie nicht (storage size of 'sa' isn't known) */
#define _POSIX_C_SOURCE 200809L // NOLINT(bugprone-reserved-identifier)

#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <time.h>
#include <unistd.h>

#include "config.h"
#include "debug.h"
#include "draw.h"
#include "input.h"
#include "keys.h"
#include "session.h"
#include "state.h"
#include "theme.h"
#include "utils.h"

static AppState *g_state = NULL;

static void on_winch(int sig)
{
    (void)sig;
    if (g_state != NULL) {
        g_state->resized = 1;
    }
}

/* SIGWINCH so installieren, dass ein blockierendes read()
 * abgebrochen wird (EINTR) – genau darauf ist der key-reader
 * gebaut (keys.c: FILL_NONE). signal() wuerde auf glibc
 * SA_RESTART setzen: read() startet danach neu und der
 * resize-wird NIE bemerkt, bis eine taste kommt. */
static void winch_install(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_winch;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0; /* bewusst KEIN SA_RESTART */
    (void)sigaction(SIGWINCH, &sa, NULL);
}

/* ------------------------------------------------------------------ */
/* resize-debounce: terminals feuern SIGWINCH in salven, waehrend der */
/* user das fenster zieht. wir warten RESIZE_DEBOUNCE_MS stillstand  */
/* und rendern erst dann – sonst zeichnen wir fuer jede zwischen-     */
/* groesse einen frame. das terminal wird zwischendurch ohnehin       */
/* umgebrochen, ein zu frueher frame waere gleich wieder muell.      */
/* ------------------------------------------------------------------ */
#define RESIZE_DEBOUNCE_MS 250

/* auf stillstand der resize-salve warten. rueckgabe: true, wenn
 * nach der frist wieder schluessel-input kam (der benutzer tippt
 * schneller als das fenster endgueltig still steht – die taste
 * wird in keys abgelegt und geht nicht verloren). */
static void resize_wait_settled(void)
{
    long long deadline = mono_ms() + RESIZE_DEBOUNCE_MS;
    for (;;) {
        long long now = mono_ms();
        if (now >= deadline) {
            return; /* salve still: jetzt ist die groesse echt */
        }
        /* jede neue winch verschiebt die frist nach hinten –
         * solange gezogen wird, wird nicht gezeichnet */
        if (g_state->resized) {
            g_state->resized = 0;
            deadline = now + RESIZE_DEBOUNCE_MS;
        }
        /* 10ms schritte: fein genug, um die frist zu treffen,
         * grob genug, um den prozessor kalt zu lassen */
        struct timespec ts = {0, 10L * 1000L * 1000L};
        (void)nanosleep(&ts, NULL);
    }
}

int main(int argc, char **argv)
{
    /* --debug: trace nach /tmp (umbenannt auf die session-id,
     * sobald die erste nachricht eine session oeffnet) */
    bool debug = false;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--debug") == 0) {
            debug = true;
        }
    }
    if (debug) {
        char path[128];
        (void)snprintf(path, sizeof path, "/tmp/max-agent-debug-%ld.log",
                       (long)getpid());
        dbg_init(path);
    }
    dbg("app start (argv[0]=%s)", (argc > 0) ? argv[0] : "?");

    /* der komplette app-zustand in einem objekt: flags, dialog-
     * cursor und die chat-eingabe (input_init setzt die erste
     * leere zeile) */
    AppState state = {0};
    input_init(&state.input);

    atexit(restore);
    raw_enable();
    screen_enter();
    g_state = &state; /* kontext fuer den SIGWINCH-handler */
    winch_install();

    /* default-farben vom terminal abfragen (muss nach raw_enable
     * passieren, sonst koennen antworten nicht gelesen werden).
     * fruehe tastatur-eingaben landen im leftover und werden dem
     * keys-modul zurueckgegeben, damit nichts verloren geht. */
    char leftover[SEQ_MAX];
    size_t leftover_len = 0;
    theme_init(leftover, sizeof leftover, &leftover_len);
    if (leftover_len > 0) {
        keys_unread(leftover, leftover_len);
    }

    int rows = 24;
    int cols = 80;
    term_size(&rows, &cols);

    Config cfg;
    if (load_config(&cfg) != 0) {
        die("failed to load config");
    }
    /* theme aus der config anwenden. schlaegt das fehl (name
     * unbekannt), bleibt das aus den terminal-farben ermittelte. */
    if (cfg.theme != NULL) {
        (void)theme_select(cfg.theme);
    }

    /* sessions-verzeichnis frueh anlegen, damit /resume und das
     * anlegen der ersten session nichts mehr anlegen muessen */
    (void)session_dir_ensure();

    draw(rows, cols, &state, &cfg);

    while (!state.quit) {
        if (state.resized) {
            g_state->resized = 0;
            resize_wait_settled();
            term_size(&rows, &cols);
            dbg("resize: %dx%d", rows, cols);
            /* KEIN \x1b[2J – der verlauf lebt im terminal-
             * scrollback. nur den renderer neu verankern und den
             * sichtbaren schwanz neu drucken: der alte content
             * steht falsch umgebrochen im scrollback. der frame
             * gehoert SOFORT dazu: handle_key blockiert sonst bis
             * zur naechsten taste und der bildschirm bliebe leer */
            draw_reset(rows, true);
            draw(rows, cols, &state, &cfg);
            state.dirty = false;
        }

        handle_key(&state, &cfg, &rows, &cols);

        if (state.dirty && !state.quit) {
            draw(rows, cols, &state, &cfg);
            state.dirty = false;
        }
    }

    /* config als letzten stand sichern – auch wenn seit dem letzten
     * aendern nichts passiert ist, garantiert das den backup beim
     * beenden (auch bei ctrl+c/ctrl-q) */
    config_persist(&cfg);

    input_free(&state.input);
    chat_free(&state.chat);
    history_free(&state.history);
    session_list_free(&state.sessions);
    session_free(&state.session);
    free_config(&cfg);
    return 0;
}
