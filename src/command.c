#define _POSIX_C_SOURCE 200809L // NOLINT(bugprone-reserved-identifier)
/* unlink, write, getpid */

#include "command.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "chat.h"
#include "draw.h"
#include "editor.h"
#include "state.h"
#include "theme.h"
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
    session_end(&state->session); /* dateien bleiben unangetastet */
    chat_clear(&state->chat);     /* "start a new session" */
    /* renderer-frontier: der chat ist geleert, neu gedruckt wird
     * nur, was danach ankommt */
    draw_content_reset();
    state->ctx.dropped = 0;
    state->ctx.total_prompt = 0;
    state->ctx.total_completion = 0;
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
     * sie beim zeichnen aus dem state */
    session_list_free(&state->sessions);
    (void)session_list_load(&state->sessions);
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

/* ------------------------------------------------------------------ */
/* /system-prompt: $EDITOR mit dem prompt                              */
/* ------------------------------------------------------------------ */

/* eine notice-zeile in den chat haengen; fehler beim anhaengen
 * sind toedlich, wie ueberall. */
static void notice(AppState *state, const char *text)
{
    if (chat_append(&state->chat, CHAT_ROLE_NOTICE, text) != 0) {
        die("out of memory");
    }
}

/* nur whitespace? (leerer prompt = zurueck zur vorlage) */
static bool prompt_is_empty(const char *s)
{
    for (const char *p = s; *p != '\0'; p++) {
        if (*p != ' ' && *p != '\t' && *p != '\n' && *p != '\r') {
            return false;
        }
    }
    return true;
}

void cmd_system_prompt(AppState *state, Config *cfg, int *rows, int *cols)
{
    input_reset(&state->input);
    state->cmd_active = false;

    /* effektiven prompt holen: den text aus der config oder die
     * vorlage aus prompt.c (mit datum/cwd eingesetzt). "off" wird
     * als leerer text vorgelegt, nicht als vorlage. */
    char *original = editor_effective_prompt(cfg);
    if (original == NULL) {
        original = dup_str("");
        if (original == NULL) {
            die("out of memory");
        }
    }

    /* temp-datei mit .md-suffix (vim & co. highlight/indent).
     * mkstemp verlangt "XXXXXX" am ENDE des templates, das suffix
     * passt also nicht dorthin. stattdessen pid-basierter name
     * mit O_CREAT|O_EXCL: atomar, 0600, ein zufaellig vorhandener
     * name fuehrt zum naechsten versuch. am ende fliegt die
     * datei in jedem fall weg. */
    char path[128];
    int fd = -1;
    for (int attempt = 0; attempt < 16 && fd < 0; attempt++) {
        (void)snprintf(path, sizeof path, "/tmp/max-agent-prompt-%ld-%d.md",
                       (long)getpid(), attempt);
        fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
        if (fd < 0 && errno != EEXIST) {
            break; /* /tmp kaputt: weiter probieren ist sinnlos */
        }
    }
    if (fd < 0) {
        free(original);
        notice(state,
               "temp-datei fuer den system-prompt liess sich nicht anlegen");
        return;
    }
    size_t len = strlen(original);
    if (len > 0 && write(fd, original, len) != (ssize_t)len) {
        (void)close(fd);
        (void)unlink(path);
        free(original);
        notice(state,
               "system-prompt liess sich nicht in die temp-datei schreiben");
        return;
    }
    if (close(fd) != 0) {
        (void)unlink(path);
        free(original);
        notice(state, "temp-datei liess sich nicht schliessen");
        return;
    }

    /* rohen modus und die app-sequences aus: der editor will ein
     * normales terminal (cooked, echo, signale). stdout leeren,
     * damit nichts von uns im editor-fenster haengen bleibt, und
     * die anstehenden tasten wegwerfen – die entstanden vor dem
     * befehl und wuerden jetzt falsche dinge tun. */
    fflush(stdout);
    keys_clear_pending();
    screen_leave();
    raw_disable();

    int rc = editor_run(path);

    /* zurueck in unsere welt: raw mode an, app-sequences an,
     * groesse neu holen (der editor kann das terminal veraendert
     * haben, z.b. via :set columns). */
    raw_enable();
    screen_enter();
    if (rows != NULL && cols != NULL) {
        if (term_size(rows, cols) == 0) {
            state->resized = 0;
        }
    }

    /* ergebnis einlesen und die temp-datei sofort wegwerfen; die
     * notices haengen wir VOR dem frame an den chat, dann druckt
     * sie der rufende main-loop (dirty=true) zusammen mit dem
     * frischen schwanz. */
    char *edited = NULL;
    size_t edited_size = 0;
    const char *message = NULL;
    char line[96];
    if (rc < 0) {
        message = "editor liess sich nicht starten";
    } else if (rc != 0) {
        (void)snprintf(line, sizeof line,
                       "bearbeitung abgebrochen (editor exit-code %d)", rc);
        message = line;
    } else if (read_file(path, &edited, &edited_size) != 0) {
        message = "bearbeitete datei liess sich nicht lesen";
    }

    if (rc >= 0) {
        (void)unlink(path);
    }

    if (message == NULL && edited != NULL && strcmp(edited, original) == 0) {
        /* unverandert gespeichert (oder :q ohne speichern): die
         * vorlage bleibt vorlage. verhindert, dass ein nur
         * geoeffnetes /system-prompt das datum/cwd der vorlage
         * einfriert. */
        free(edited);
        edited = NULL;
        message = "system-prompt unveraendert";
    }
    free(original);

    if (message == NULL) {
        /* leerer text heisst "zurueck zur vorlage", konsistent mit
         * dem prompt_edit-feld in keys.c (dort ist leer ebenfalls
         * default, "off" ist der explizite weg). sonst uebernehmen. */
        char *text = NULL;
        if (edited != NULL && !prompt_is_empty(edited)) {
            text = edited;
        } else {
            free(edited);
        }
        free(cfg->system_prompt);
        cfg->system_prompt = text;
        config_persist(cfg);
        /* snapshot der offenen session nachziehen: ein resume soll
         * den prompt wiederherstellen, den die session zuletzt
         * hatte, nicht den von ihrem anfang */
        (void)session_prompt_changed(&state->session, text);

        if (text != NULL) {
            (void)snprintf(line, sizeof line,
                           "system-prompt aktualisiert (%zu zeichen)",
                           strlen(text));
            message = line;
        } else {
            message = "system-prompt zurueck auf die vorlage gesetzt";
        }
    }

    if (message != NULL) {
        notice(state, message);
    }

    /* der editor hat den sichtbaren bildschirm ueberschrieben –
     * der eigene frame von davor ist muell. bis zum boden
     * scrollen (draw_reset ohne full-reprint: der chat-inhalt
     * bleibt der alte, nur der dock-anker faellt) und danach nur
     * den schwanz drucken, wie nach /new und resume. das frame
     * selbst zeichnet der main-loop (state->dirty), wir stehen
     * hier noch in handle_key. */
    if (rows != NULL && cols != NULL) {
        draw_reset(*rows, false);
        draw_content_reset();
        state->dirty = true;
    }
}
