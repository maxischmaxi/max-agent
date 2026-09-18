#define _POSIX_C_SOURCE 200809L // NOLINT(bugprone-reserved-identifier)

#include "command.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "chat.h"
#include "draw.h"
#include "keys.h"
#include "state.h"
#include "utils.h"

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

int cmd_list_height(const AppState *st)
{
    if (!st->cmd_active) {
        return 0;
    }
    char prefix[64];
    int idx[COMMAND_COUNT];
    cmd_prefix(&st->input, prefix, sizeof prefix);
    int n = cmd_match(prefix, idx, COMMAND_COUNT);
    return (n > 0) ? n : 1; /* kein treffer: hinweis-zeile */
}

/* neue session beginnen: die aktuelle bleibt auf der platte liegen,
 * wie sie ist (session_end schliesst nur), und der chat startet
 * neu. /clear tut exakt dasselbe. die ctx-gesamtzaehler gehoeren
 * zur sitzung und starten damit neu; die eichung (scale) bleibt,
 * sie ist eine eigenschaft des schaetzers, nicht des verlaufs. */
void cmd_new(AppState *state)
{
    input_reset(&state->input);   /* draw() schreibt eh jeden frame */
    keys_queue_clear(state);      /* gebuffertes gehoert zum verlauf */
    session_end(&state->session); /* dateien bleiben unangetastet */
    chat_clear(&state->chat);     /* "start a new session" */
    /* renderer-frontier: der chat ist geleert, neu gedruckt wird
     * nur, was danach ankommt */
    draw_content_reset();
    state->ctx.dropped = 0;
    state->ctx.last_prompt = 0;
    state->ctx.last_cached = 0;
    state->ctx.total_completion = 0;
    ctx_reset(&state->ctx); /* compaction-summary gehoert zum verlauf */
    state->worked_ms = 0;
    state->busy_start_ms = 0;
}

void cmd_clear(AppState *state)
{
    cmd_new(state);
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

void cmd_resume(AppState *state)
{
    state->sessions_dialog = true;
    state->dialog = (DialogState){0}; /* frisch: leere suche */
    input_reset(&state->input);
    /* liste frisch laden (alte vorher wegwerfen): das dialog liest
     * sie beim zeichnen aus dem state. nur die sessionen des
     * ordners, in dem die app laeuft – sessionen anderer projekte
     * bleiben dort unsichtbar, wo sie hingehoeren. */
    session_list_free(&state->sessions);
    (void)session_list_load(&state->sessions, NULL);
}

void cmd_rename(AppState *state, const Config *cfg, const char *name)
{
    input_reset(&state->input);

    /* fuehrende/folgende leerzeichen weg, leeres feld = benutzungs-
     * hinweis statt stiller fehler */
    const char *p = (name != NULL) ? name : "";
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    size_t len = strlen(p);
    while (len > 0 && (p[len - 1] == ' ' || p[len - 1] == '\t')) {
        len--;
    }
    if (len == 0) {
        if (chat_append(&state->chat, CHAT_ROLE_NOTICE,
                        "benutzung: /rename <neuer-name>") != 0) {
            die("out of memory");
        }
        return;
    }

    /* noch keine session offen: hier erzeugen – erst nur mit id,
     * dann den namen draufsetzen (genau wie bei /rename einer
     * laufenden session) */
    if (!state->session.active && session_start(&state->session, cfg) != 0) {
        if (chat_append(&state->chat, CHAT_ROLE_ERROR,
                        "session liess sich nicht anlegen") != 0) {
            die("out of memory");
        }
        return;
    }

    char trimmed[256];
    if (len >= sizeof trimmed) {
        len = sizeof trimmed - 1; /* der name muss aufs terminal */
    }
    memcpy(trimmed, p, len);
    trimmed[len] = '\0';

    if (session_rename(&state->session, trimmed) != 0) {
        if (chat_append(&state->chat, CHAT_ROLE_ERROR,
                        "umbenennen fehlgeschlagen") != 0) {
            die("out of memory");
        }
        return;
    }

    char line[300];
    (void)snprintf(line, sizeof line, "session umbenannt: %s", trimmed);
    if (chat_append(&state->chat, CHAT_ROLE_NOTICE, line) != 0) {
        die("out of memory");
    }
}
