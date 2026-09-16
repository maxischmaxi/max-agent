#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/select.h>
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
    signal(SIGWINCH, on_winch);

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
        state.dirty = (state.resized != 0);
        if (state.resized) {
            state.resized = 0;
            term_size(&rows, &cols);
            dbg("resize: %dx%d", rows, cols);
            /* KEIN \x1b[2J – der verlauf lebt im terminal-
             * scrollback. nur den renderer neu verankern */
            draw_reset(rows);
        }

        handle_key(&state, &cfg, rows, cols);

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
