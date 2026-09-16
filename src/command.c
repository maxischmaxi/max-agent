#include "command.h"

#include <string.h>

#include "chat.h"
#include "state.h"

int cmd_name_col(void)
{
    int w = 0;
    for (int i = 0; i < COMMAND_COUNT; i++) {
        int len = (int)strlen(COMMANDS[i].name);
        if (len > w) {
            w = len;
        }
    }
    return w;
}

int cmd_match(const char *prefix, int *out, int out_max)
{
    int n = 0;
    size_t plen = strlen(prefix);
    for (int i = 0; i < COMMAND_COUNT && n < out_max; i++) {
        if (strncmp(COMMANDS[i].name, prefix, plen) == 0) {
            out[n++] = i;
        }
    }
    return n;
}

int cmd_lookup(const char *word)
{
    if (word == NULL || word[0] == '\0') {
        return -1;
    }
    const char *name = (word[0] == '/') ? word + 1 : word;
    for (int i = 0; i < COMMAND_COUNT; i++) {
        if (strcmp(COMMANDS[i].name, name) == 0) {
            return i;
        }
    }
    return -1;
}

void cmd_clear(AppState *state)
{
    input_reset(&state->input); /* draw() schreibt eh jeden frame
                                 * komplett, "clear" = input leeren */
    chat_clear(&state->chat);   /* "start a new session": verlauf weg */
    state->chat_scroll = 0;
    state->ctx.dropped = 0; /* neuer verlauf, nichts mehr gekuerzt */
}

void cmd_models(AppState *state)
{
    state->models_dialog = true;
    state->dialog = (DialogState){0}; /* frisch: leere suche */
    input_reset(&state->input);
}

void cmd_settings(AppState *state)
{
    state->settings_dialog = true;
    state->theme_sub = false;
    state->dialog = (DialogState){0}; /* frisch: leere suche */
    input_reset(&state->input);
}
