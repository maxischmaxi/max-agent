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

int main(void)
{
    /* der komplette app-zustand in einem objekt: flags, dialog-
     * cursor und die chat-eingabe (input_init setzt die erste
     * leere zeile) */
    AppState state = {0};
    input_init(&state.input);

    /* debug-zustand gehoert main und wird als pointer durchgereicht.
     * static: ~82KB, zero-initialisiert, kein stack-verbrauch. */
    static DebugState dbg;

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
    dbg_log(&dbg, "theme: %s (match=%s)", theme_current()->name,
            theme_current()->match);

    int rows = 24;
    int cols = 80;
    term_size(&rows, &cols);

    Config cfg;
    if (load_config(&cfg) != 0) {
        die("failed to load config");
    }
    /* theme aus der config anwenden (fehlt/unbekannt: wie ermittelt) */
    if (cfg.theme != NULL && !theme_select(cfg.theme)) {
        dbg_log(&dbg, "unbekanntes theme in config: '%s'", cfg.theme);
    }
    dbg_log(&dbg, "debug sidebar aktiv (%dx%d)", cols, rows);

    draw(rows, cols, &state, &dbg, &cfg);

    while (!state.quit) {
        state.dirty = (state.resized != 0);
        if (state.resized) {
            state.resized = 0;
            term_size(&rows, &cols);
            fputs("\x1b[2J", stdout);
        }

        handle_key(&state, &cfg, &dbg, rows, cols);

        if (state.dirty && !state.quit) {
            draw(rows, cols, &state, &dbg, &cfg);
            state.dirty = false;
        }
    }

    /* config als letzten stand sichern – auch wenn seit dem letzten
     * aendern nichts passiert ist, garantiert das den backup beim
     * beenden (auch bei ctrl+c/ctrl-q) */
    config_persist(&cfg, &dbg);

    input_free(&state.input);
    chat_free(&state.chat);
    history_free(&state.history);
    free_config(&cfg);
    return 0;
}
