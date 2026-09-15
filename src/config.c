#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "debug.h"
#include "utils.h"

static const char *input_type_to_str(InputType t)
{
    switch (t) {
    case TEXT:
        return "text";
    case IMAGE:
        return "image";
    case VIDEO:
        return "video";
    }
    return "text";
}

static bool input_type_from_str(const char *s, InputType *out)
{
    if (strcmp(s, "text") == 0) {
        *out = TEXT;
        return true;
    }
    if (strcmp(s, "image") == 0) {
        *out = IMAGE;
        return true;
    }
    if (strcmp(s, "video") == 0) {
        *out = VIDEO;
        return true;
    }
    return false;
}

static const char *api_to_str(API api)
{
    switch (api) {
    case OPENAI_COMPLETIONS:
        return "openai-completions";
    }
    return "";
}

static bool api_from_str(const char *s, API *out)
{
    if (strcmp(s, "openai-completions") == 0) {
        *out = OPENAI_COMPLETIONS;
        return true;
    }
    return false;
}

static int dup_json_str(cJSON *obj, const char *key, char **out)
{
    cJSON *it = cJSON_GetObjectItem(obj, key);
    if (cJSON_IsString(it) && it->valuestring != NULL) {
        char *s = dup_str(it->valuestring);
        if (!s) {
            return -1;
        }
        *out = s;
    }
    return 0;
}

static int load_model(Model *m, cJSON *item)
{
    cJSON *ctx = cJSON_GetObjectItem(item, "contextWindow");
    if (cJSON_IsNumber(ctx)) {
        if (ctx->valuedouble < 0) {
            return -1;
        }
        m->context_window = (size_t)ctx->valuedouble;
    }

    if (dup_json_str(item, "id", &m->id) != 0) {
        return -1;
    }
    if (m->id) {
        m->id_len = strlen(m->id);
    }

    cJSON *reasoning = cJSON_GetObjectItem(item, "reasoning");
    if (cJSON_IsBool(reasoning)) {
        m->reasoning = (cJSON_IsTrue(reasoning) != 0);
    }

    cJSON *inputs = cJSON_GetObjectItem(item, "input");
    if (cJSON_IsArray(inputs)) {
        size_t count = 0;
        cJSON *j;
        cJSON_ArrayForEach(j, inputs)
        {
            if (cJSON_IsString(j)) {
                count++;
            }
        }
        if (count > 0) {
            m->inputTypes = calloc(count, sizeof(InputType));
            if (!m->inputTypes) {
                return -1;
            }
            cJSON_ArrayForEach(j, inputs)
            {
                if (m->input_types_len >= count) {
                    break;
                }
                if (!cJSON_IsString(j) || j->valuestring == NULL) {
                    continue;
                }
                InputType t;
                if (!input_type_from_str(j->valuestring, &t)) {
                    return -1;
                }
                m->inputTypes[m->input_types_len++] = t;
            }
        }
    }
    return 0;
}

static int load_provider(Provider *pro, cJSON *item)
{
    cJSON *api = cJSON_GetObjectItem(item, "api");
    if (cJSON_IsString(api) && api->valuestring != NULL) {
        if (!api_from_str(api->valuestring, &pro->api)) {
            return -1;
        }
    }

    if (dup_json_str(item, "apiKey", &pro->api_key) != 0) {
        return -1;
    }
    if (dup_json_str(item, "baseUrl", &pro->base_url) != 0) {
        return -1;
    }

    cJSON *models = cJSON_GetObjectItem(item, "models");
    if (!cJSON_IsArray(models)) {
        return 0;
    }

    size_t count = 0;
    cJSON *j;
    cJSON_ArrayForEach(j, models)
    {
        if (cJSON_IsObject(j)) {
            count++;
        }
    }
    if (count == 0) {
        return 0;
    }

    pro->models = calloc(count, sizeof(Model));
    if (!pro->models) {
        return -1;
    }

    cJSON_ArrayForEach(j, models)
    {
        if (!cJSON_IsObject(j)) {
            continue;
        }
        if (load_model(&pro->models[pro->models_len], j) != 0) {
            return -1;
        }
        pro->models_len++;
    }
    return 0;
}

static void load_settings(cJSON *root, Config *config)
{
    /* ein fehlendes "settings"-objekt ist kein fehler: defaults
     * stehen schon (siehe load_config_from) */
    cJSON *s = cJSON_GetObjectItem(root, "settings");
    if (!cJSON_IsObject(s)) {
        return;
    }
    (void)dup_json_str(s, "theme", &config->theme);
    (void)dup_json_str(s, "activeModel", &config->active_model);

    cJSON *cq = cJSON_GetObjectItem(s, "confirmQuit");
    if (cJSON_IsBool(cq)) {
        config->confirm_quit = (cJSON_IsTrue(cq) != 0);
    }
}

int load_config_from(const char *path, Config *config)
{
    memset(config, 0, sizeof *config);
    config->confirm_quit =
        true; /* default, kann von "settings" ueberschrieben werden */

    if (!is_file(path)) {
        return 0; /* keine datei -> leere config, kein fehler */
    }

    cJSON *root = parse_json_file(path);
    if (root == NULL || !cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return -1;
    }

    cJSON *providers = cJSON_GetObjectItem(root, "providers");
    if (!cJSON_IsArray(providers)) {
        cJSON_Delete(root);
        return -1;
    }

    size_t count = 0;
    cJSON *j;
    cJSON_ArrayForEach(j, providers)
    {
        if (cJSON_IsObject(j)) {
            count++;
        }
    }

    if (count > 0) {
        config->providers = calloc(count, sizeof(Provider));
        if (!config->providers) {
            cJSON_Delete(root);
            return -1;
        }
        cJSON_ArrayForEach(j, providers)
        {
            if (!cJSON_IsObject(j)) {
                continue;
            }
            if (load_provider(&config->providers[config->providers_len], j) !=
                0) {
                cJSON_Delete(root);
                free_config(config);
                return -1;
            }
            config->providers_len++;
        }
    }

    load_settings(root, config);
    cJSON_Delete(root);
    return 0;
}

int load_config(Config *config)
{
    char *path = append_to_home(".config/.maxagent/config.json");
    if (!path) {
        return -1;
    }
    int rc = load_config_from(path, config);
    free(path);
    return rc;
}

static cJSON *model_to_json(const Model *m)
{
    cJSON *obj = cJSON_CreateObject();
    if (!obj) {
        return NULL;
    }

    if (m->id != NULL) {
        cJSON *id = cJSON_CreateString(m->id);
        if (!id) {
            goto fail;
        }
        cJSON_AddItemToObject(obj, "id", id);
    }

    cJSON *ctx = cJSON_CreateNumber((double)m->context_window);
    if (!ctx) {
        goto fail;
    }
    cJSON_AddItemToObject(obj, "contextWindow", ctx);

    cJSON *reasoning = cJSON_CreateBool((cJSON_bool)m->reasoning);
    if (!reasoning) {
        goto fail;
    }
    cJSON_AddItemToObject(obj, "reasoning", reasoning);

    if (m->inputTypes != NULL && m->input_types_len > 0) {
        cJSON *arr = cJSON_CreateArray();
        if (!arr) {
            goto fail;
        }
        for (size_t i = 0; i < m->input_types_len; i++) {
            cJSON *s = cJSON_CreateString(input_type_to_str(m->inputTypes[i]));
            if (!s) {
                cJSON_Delete(arr);
                goto fail;
            }
            cJSON_AddItemToArray(arr, s);
        }
        cJSON_AddItemToObject(obj, "input", arr);
    }

    return obj;

fail:
    cJSON_Delete(obj);
    return NULL;
}

static cJSON *provider_to_json(const Provider *p)
{
    cJSON *obj = cJSON_CreateObject();
    if (!obj) {
        return NULL;
    }

    cJSON *api = cJSON_CreateString(api_to_str(p->api));
    if (!api) {
        goto fail;
    }
    cJSON_AddItemToObject(obj, "api", api);

    if (p->api_key != NULL) {
        cJSON *key = cJSON_CreateString(p->api_key);
        if (!key) {
            goto fail;
        }
        cJSON_AddItemToObject(obj, "apiKey", key);
    }

    if (p->base_url != NULL) {
        cJSON *url = cJSON_CreateString(p->base_url);
        if (!url) {
            goto fail;
        }
        cJSON_AddItemToObject(obj, "baseUrl", url);
    }

    cJSON *models = cJSON_CreateArray();
    if (!models) {
        goto fail;
    }
    for (size_t i = 0; i < p->models_len; i++) {
        cJSON *m = model_to_json(&p->models[i]);
        if (!m) {
            cJSON_Delete(models);
            goto fail;
        }
        cJSON_AddItemToArray(models, m);
    }
    cJSON_AddItemToObject(obj, "models", models);

    return obj;

fail:
    cJSON_Delete(obj);
    return NULL;
}

static cJSON *settings_to_json(const Config *config)
{
    cJSON *obj = cJSON_CreateObject();
    if (!obj) {
        return NULL;
    }

    cJSON *theme =
        cJSON_CreateString((config->theme != NULL) ? config->theme : "auto");
    if (!theme) {
        goto fail;
    }
    cJSON_AddItemToObject(obj, "theme", theme);

    cJSON *confirm = cJSON_CreateBool((cJSON_bool)config->confirm_quit);
    if (!confirm) {
        goto fail;
    }
    cJSON_AddItemToObject(obj, "confirmQuit", confirm);

    if (config->active_model != NULL) {
        cJSON *model = cJSON_CreateString(config->active_model);
        if (!model) {
            goto fail;
        }
        cJSON_AddItemToObject(obj, "activeModel", model);
    }

    return obj;

fail:
    cJSON_Delete(obj);
    return NULL;
}

int save_config_to(const char *path, const Config *config)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return -1;
    }

    cJSON *providers = cJSON_CreateArray();
    if (!providers) {
        cJSON_Delete(root);
        return -1;
    }

    for (size_t i = 0; i < config->providers_len; i++) {
        cJSON *p = provider_to_json(&config->providers[i]);
        if (!p) {
            cJSON_Delete(providers);
            cJSON_Delete(root);
            return -1;
        }
        cJSON_AddItemToArray(providers, p);
    }
    cJSON_AddItemToObject(root, "providers", providers);

    cJSON *settings = settings_to_json(config);
    if (!settings) {
        cJSON_Delete(root);
        return -1;
    }
    cJSON_AddItemToObject(root, "settings", settings);

    char *json = cJSON_Print(root);
    cJSON_Delete(root);
    if (!json) {
        return -1;
    }

    int rc = write_file(path, json, strlen(json));
    cJSON_free(json);
    return rc == 0 ? 0 : -1;
}

int save_config(const Config *config)
{
    char *dir = append_to_home(".config/.maxagent");
    if (!dir) {
        return -1;
    }
    if (!is_dir(dir)) {
        if (mkdir_p(dir, 0755) != 0) {
            free(dir);
            return -1;
        }
    }
    free(dir);

    char *path = append_to_home(".config/.maxagent/config.json");
    if (!path) {
        return -1;
    }
    int rc = save_config_to(path, config);
    free(path);
    return rc;
}

void free_config(Config *config)
{
    if (config == NULL) {
        return;
    }
    free(config->theme);
    config->theme = NULL;
    free(config->active_model);
    config->active_model = NULL;

    if (config->providers == NULL) {
        return;
    }
    for (size_t i = 0; i < config->providers_len; i++) {
        Provider *p = &config->providers[i];
        free(p->api_key);
        free(p->base_url);
        for (size_t k = 0; k < p->models_len; k++) {
            free(p->models[k].id);
            free(p->models[k].inputTypes);
        }
        free(p->models);
    }
    free(config->providers);
    config->providers = NULL;
    config->providers_len = 0;
}

void config_persist(Config *cfg, DebugState *dbg)
{
    (void)dbg; /* dbg_log: im release wegkompiliert */
    if (save_config(cfg) == 0) {
        dbg_log(dbg, "config gespeichert");
    } else {
        dbg_log(dbg, "config speichern fehlgeschlagen");
    }
}
