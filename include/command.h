#ifndef MAX_AGENT_COMMAND
#define MAX_AGENT_COMMAND

#include "config.h"
#include "input.h"
#include "state.h"
#define COMMAND_CLEAR         "clear"
#define COMMAND_QUIT          "quit"
#define COMMAND_MODELS        "models"
#define COMMAND_SETTINGS      "settings"
#define COMMAND_NEW           "new"
#define COMMAND_RESUME        "resume"
#define COMMAND_SESSIONS      "sessions"
#define COMMAND_RENAME        "rename"
#define COMMAND_SYSTEM_PROMPT "system-prompt"

typedef enum {
    CMD_CLEAR = 0,
    CMD_MODELS,
    CMD_NEW,
    CMD_QUIT,
    CMD_RENAME,
    CMD_RESUME,
    CMD_SESSIONS, /* alias: oeffnet denselben dialog wie resume */
    CMD_SETTINGS,
    CMD_SYSTEM_PROMPT, /* $EDITOR mit dem system-prompt oeffnen */
    CMD_COUNT, /* muss immer letzter sein: COMMANDS[] sonst NULL-luecke */
} CmdId;

typedef struct {
    const char *name;
    const char *desc;
} Command;

static const Command COMMANDS[] = {
    [CMD_CLEAR] = {COMMAND_CLEAR, "start a new session"},
    [CMD_MODELS] = {COMMAND_MODELS, "select a model"},
    [CMD_NEW] = {COMMAND_NEW, "start a new session"},
    [CMD_QUIT] = {COMMAND_QUIT, "quit max agent"},
    [CMD_RENAME] = {COMMAND_RENAME, "name the current session"},
    [CMD_RESUME] = {COMMAND_RESUME, "list and resume sessions"},
    [CMD_SESSIONS] = {COMMAND_SESSIONS, "list and resume sessions"},
    [CMD_SETTINGS] = {COMMAND_SETTINGS, "max agent settings"},
    [CMD_SYSTEM_PROMPT] = {COMMAND_SYSTEM_PROMPT,
                           "edit the system prompt in your editor"},
};

#define COMMAND_COUNT ((int)(sizeof COMMANDS / sizeof COMMANDS[0]))

int cmd_lookup(const char *word);
int cmd_name_col(void);
int cmd_match(const char *prefix, int *out, int out_max);

/* hoehe der befehlsliste unter dem eingabefeld (0 = geschlossen):
 * treffer-zeilen oder eine hinweis-zeile. keys.c braucht sie fuer
 * die wachstumsentscheidung der eingabe, der dock fuer seine
 * geometrie – beide muessen dieselbe zahl sehen. */
int cmd_list_height(const AppState *st);

/* dialog-oeffner: setzen die dialog-flags im state und leeren die
 * chat-eingabe (das input liegt als member im state) */
void cmd_new(AppState *state);
void cmd_clear(AppState *state);
void cmd_models(AppState *state);
void cmd_settings(AppState *state);
void cmd_resume(AppState *state);

/* den effektiven system-prompt in eine temp-datei schreiben und
 * im $VISUAL/$EDITOR oeffnen. :wq uebernimmt den neuen text in
 * die config (und den session-snapshot), :q/:cq aendert nichts.
 * rows/cols zeigen auf die terminalgroesse der aufrufenden
 * schleife: nach dem editor wird damit der schwanz neu gezeichnet
 * (draw_content_reset) – der editor bleibt im scrollback. */
void cmd_system_prompt(AppState *state, Config *cfg, int *rows, int *cols);

/* die AKTUELLE session benennen. ist noch keine offen, entsteht
 * sie hier (erst nur mit id, ohne nachrichten). leerer name ist
 * ein fehler -> hinweis im verlauf. */
void cmd_rename(AppState *state, const Config *cfg, const char *name);

#endif
