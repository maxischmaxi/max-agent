#ifndef MAX_AGENT_COMMAND
#define MAX_AGENT_COMMAND

#define COMMAND_CLEAR    "clear"
#define COMMAND_QUIT     "quit"
#define COMMAND_MODELS   "models"
#define COMMAND_SETTINGS "settings"

typedef enum {
    CMD_CLEAR = 0,
    CMD_MODELS,
    CMD_QUIT,
    CMD_SETTINGS,
    CMD_COUNT, /* muss immer letzter sein: COMMANDS[] sonst NULL-luecke */
} CmdId;

typedef struct {
    const char *name;
    const char *desc;
} Command;

static const Command COMMANDS[] = {
    [CMD_CLEAR] = {COMMAND_CLEAR, "start a new session"},
    [CMD_MODELS] = {COMMAND_MODELS, "select a model"},
    [CMD_QUIT] = {COMMAND_QUIT, "quit max agent"},
    [CMD_SETTINGS] = {COMMAND_SETTINGS, "max agent settings"},
};

#define COMMAND_COUNT ((int)(sizeof COMMANDS / sizeof COMMANDS[0]))

int cmd_lookup(const char *word);
int cmd_name_col(void);
int cmd_match(const char *prefix, int *out, int out_max);

#endif
