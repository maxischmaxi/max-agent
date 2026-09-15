#ifndef MAX_AGENT_CONFIG
#define MAX_AGENT_CONFIG

#include <stdbool.h>
#include <stddef.h>

#include "debug.h"

typedef enum {
    TEXT,
    IMAGE,
    VIDEO,
} InputType;

typedef struct {
    size_t context_window;
    char *id;
    size_t id_len;
    InputType *inputTypes;
    size_t input_types_len;
    bool reasoning;
} Model;

typedef enum {
    OPENAI_COMPLETIONS,
} API;

typedef struct {
    API api;
    char *api_key;
    char *base_url;
    Model *models;
    size_t models_len;
} Provider;

typedef struct {
    Provider *providers;
    size_t providers_len;

    /* settings (settings-dialog, in der config unter "settings") */
    char *theme;        /* theme-name oder NULL (= "auto") */
    bool confirm_quit;  /* ctrl+c doppelt bestaetigen? (default: an) */
    char *active_model; /* id des im models-dialog gewaehlten modells */
} Config;

int load_config(Config *config);
int save_config(const Config *config);

int load_config_from(const char *path, Config *config);
int save_config_to(const char *path, const Config *config);

void free_config(Config *config);
void config_persist(Config *cfg, DebugState *dbg);

#endif
