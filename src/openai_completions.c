/* ------------------------------------------------------------------ */
/* openai_completions: implementierung, siehe header.                  */
/*                                                                     */
/* http-schicht: libcurl (easy handle pro anfrage), json: cJSON.       */
/* stream: server-sent events (sse), ein chunk pro "data:"-line,       */
/* "data: [DONE]" beendet den strom (wie im npm-package).              */
/* ------------------------------------------------------------------ */

#define _POSIX_C_SOURCE 200809L // NOLINT(bugprone-reserved-identifier)

#include "openai_completions.h"

#include <curl/curl.h>
#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "cJSON.h"

#define OAI_DEFAULT_BASE_URL          "https://api.openai.com/v1"
#define OAI_DEFAULT_TIMEOUT_MS        600000L
#define OAI_DEFAULT_MAX_RETRIES       2
#define OAI_USER_AGENT                "openai-completions-c/0.1.0"
#define OAI_STREAM_CONNECT_TIMEOUT_MS 10000L
#define OAI_DEFAULT_STREAM_IDLE_MS    300000L /* 5 min ohne daten = tot */
#define OAI_DEFAULT_MAX_BODY_BYTES    (32L * 1024L * 1024L) /* 32 MiB */

/* ------------------------------------------------------------------ */
/* hilfsfunktionen                                                     */
/* ------------------------------------------------------------------ */

static char *dup_cstr(const char *s)
{
    if (s == NULL) {
        return NULL;
    }
    size_t n = strlen(s) + 1;
    char *d = malloc(n);
    if (d != NULL) {
        memcpy(d, s, n);
    }
    return d;
}

/* string-feld als heap-kopie, NULL wenn feld fehlt/kein string ist */
static char *get_string_dup(const cJSON *obj, const char *key)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (!cJSON_IsString(item) || item->valuestring == NULL) {
        return NULL;
    }
    return dup_cstr(item->valuestring);
}

static int get_int(const cJSON *obj, const char *key, int def)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (!cJSON_IsNumber(item)) {
        return def;
    }
    return item->valueint;
}

/* 0.5s * 2^attempt, deckel bei 8s (npm: exponential backoff). wenn der   */
/* server einen gueltigen retry-after (0..60s) schickt, gehorchen wir      */
/* ihm wie das npm-package. http-datum-format wird nicht unterstuetzt,     */
/* dann gilt das default-backoff (openai schickt nur sekunden).           */
/* monotoner uhrzeitstempel in sekunden (fuer den idle-watchdog) */
static double mono_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ((double)ts.tv_nsec / 1e9);
}

static void backoff_sleep(int attempt, const char *retry_after)
{
    long ms = -1;
    if (retry_after != NULL && retry_after[0] != '\0') {
        char *end = NULL;
        errno = 0;
        double seconds = strtod(retry_after, &end);
        if (errno == 0 && end != NULL && *end == '\0' && seconds >= 0.0 &&
            seconds < 60.0) {
            ms = (long)(seconds * 1000.0);
        }
    }
    if (ms < 0) {
        long base = 500L << (attempt > 4 ? 4 : attempt);
        /* npm-paritaet: jitter, bis zu 25% abziehen. entropie aus der   */
        /* uhr, damit parallele clients nicht synchron warten           */
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        double jitter = 1.0 - ((double)(now.tv_nsec % 250) / 1000.0);
        ms = (long)((double)base * jitter);
    }
    struct timespec ts = {
        .tv_sec = (time_t)(ms / 1000L),
        .tv_nsec = (ms % 1000L) * 1000000L,
    };
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {
        /* signalunterbrechung: restzeit weiter schlafen */
    }
}

/* curl_global_init/cleanup referenzzaehlen, damit mehrere clients      */
/* ueber die laufzeit des programms moeglich sind                       */
static int g_curl_refs = 0;

static int curl_ensure_global(void)
{
    if (g_curl_refs == 0 && curl_global_init(CURL_GLOBAL_DEFAULT) != 0) {
        return -1;
    }
    g_curl_refs++;
    return 0;
}

static void curl_release_global(void)
{
    if (g_curl_refs <= 0) {
        return;
    }
    g_curl_refs--;
    if (g_curl_refs == 0) {
        curl_global_cleanup();
    }
}

/* ------------------------------------------------------------------ */
/* anwachsender puffer fuer http-antworten + sse-parsing               */
/* ------------------------------------------------------------------ */

typedef struct {
    char *buf; /* immer null-terminiert */
    size_t len;
    size_t cap;
} Buf;

static int buf_reserve(Buf *b, size_t extra)
{
    if (b->len + extra + 1 <= b->cap) {
        return 0;
    }
    size_t cap = b->cap == 0 ? 4096 : b->cap;
    while (cap < b->len + extra + 1) {
        cap *= 2;
    }
    char *buf = realloc(b->buf, cap);
    if (buf == NULL) {
        return -1;
    }
    b->buf = buf;
    b->cap = cap;
    return 0;
}

static int buf_append(Buf *b, const char *data, size_t n)
{
    if (n == 0) {
        return 0;
    }
    if (buf_reserve(b, n) != 0) {
        return -1;
    }
    memcpy(b->buf + b->len, data, n);
    b->len += n;
    b->buf[b->len] = '\0';
    return 0;
}

/* ------------------------------------------------------------------ */
/* request-state                                                       */
/* ------------------------------------------------------------------ */

typedef struct {
    const OaiClient *client;
    const OaiChatCompletionParams *params;
    bool stream;
    OaiStreamCallbacks cb;
    OaiCompletionResult *result;

    Buf *body;            /* gesamter antwort-koerper */
    size_t sse_pos;       /* konsumierter offset im body (fuer sse) */
    bool sse_done;        /* [DONE] gesehen */
    bool aborted;         /* on_chunk hat abbruch verlangt */
    bool overflow;        /* body-limit ueberschritten */
    bool no_retry;        /* deterministischer fehler, retry unnoetig */
    bool idle_hit;        /* stream-idle-timeout ausgeloest */
    double last_activity; /* letzter datenempfang (mono_now) */
    char retry_after[32]; /* wert aus retry-after(-ms)-header */
    size_t chunks_delivered;
} RequestState;

/* ------------------------------------------------------------------ */
/* validierung + json-aufbau der anfrage (params -> cJSON)             */
/* ------------------------------------------------------------------ */

/* pflichtfelder pruefen, bevor wir sie in json bauen. ohne das        */
/* wuerde ein fehlendes feld erst als verwirrende server-antwort       */
/* oder bei NULL in cJSON als "out of memory" auffallen.               */
static int params_validate(const OaiChatCompletionParams *p)
{
    if (p->model == NULL || p->messages == NULL || p->messages_len == 0) {
        return -1;
    }
    for (size_t i = 0; i < p->messages_len; i++) {
        const OaiMessage *msg = &p->messages[i];
        if (msg->content == NULL && msg->tool_calls_len == 0 &&
            msg->content_parts_len == 0) {
            return -1; /* nachricht ohne inhalt */
        }
        if (msg->content != NULL && msg->content_parts_len > 0) {
            return -1; /* content und content_parts gleichzeitig */
        }
        if (msg->content_parts_len > 0 && msg->content_parts == NULL) {
            return -1;
        }
        for (size_t j = 0; j < msg->content_parts_len; j++) {
            const OaiContentPart *part = &msg->content_parts[j];
            if (part->type == OAI_CONTENT_TEXT) {
                if (part->text == NULL) {
                    return -1;
                }
            } else if (part->type == OAI_CONTENT_IMAGE_URL) {
                if (part->image_url == NULL) {
                    return -1;
                }
            } else {
                return -1; /* ungueltiger part-type */
            }
        }
        if (msg->tool_calls_len > 0 && msg->tool_calls == NULL) {
            return -1;
        }
        for (size_t j = 0; j < msg->tool_calls_len; j++) {
            const OaiToolCall *call = &msg->tool_calls[j];
            if (call->id == NULL || call->name == NULL ||
                call->arguments == NULL) {
                return -1;
            }
        }
    }
    if (p->stop_len > 0 && p->stop == NULL) {
        return -1;
    }
    for (size_t i = 0; i < p->stop_len; i++) {
        if (p->stop[i] == NULL) {
            return -1;
        }
    }
    if (p->tools_len > 0 && p->tools == NULL) {
        return -1;
    }
    for (size_t i = 0; i < p->tools_len; i++) {
        if (p->tools[i].function.name == NULL) {
            return -1;
        }
    }
    if (p->has_tool_choice) {
        if (p->tool_choice == OAI_TOOL_CHOICE_FUNCTION) {
            if (p->tool_choice_function == NULL) {
                return -1;
            }
        } else if (p->tool_choice_function != NULL) {
            return -1; /* name nur bei FUNCTION sinnvoll */
        }
    }
    if (p->has_response_format && p->response_format_type == NULL) {
        return -1;
    }
    return 0;
}

static const char *role_str(OaiRole role)
{
    switch (role) {
    case OAI_ROLE_DEVELOPER:
        return "developer";
    case OAI_ROLE_SYSTEM:
        return "system";
    case OAI_ROLE_USER:
        return "user";
    case OAI_ROLE_ASSISTANT:
        return "assistant";
    case OAI_ROLE_TOOL:
        return "tool";
    }
    return "user";
}

static cJSON *message_to_json(const OaiMessage *msg)
{
    cJSON *m = cJSON_CreateObject();
    if (m == NULL) {
        return NULL;
    }
    if (cJSON_AddStringToObject(m, "role", role_str(msg->role)) == NULL) {
        goto fail;
    }
    if (msg->content_parts_len > 0) {
        /* multimodaler content: array aus text- und image-parts (vision) */
        cJSON *parts = cJSON_AddArrayToObject(m, "content");
        if (parts == NULL) {
            goto fail;
        }
        for (size_t i = 0; i < msg->content_parts_len; i++) {
            const OaiContentPart *part = &msg->content_parts[i];
            cJSON *obj = cJSON_CreateObject();
            if (obj == NULL) {
                goto fail;
            }
            if (part->type == OAI_CONTENT_IMAGE_URL) {
                bool ok =
                    cJSON_AddStringToObject(obj, "type", "image_url") != NULL;
                cJSON *img = NULL;
                if (ok) {
                    img = cJSON_AddObjectToObject(obj, "image_url");
                    ok = img != NULL;
                }
                if (ok && cJSON_AddStringToObject(img, "url",
                                                  part->image_url) == NULL) {
                    ok = false;
                }
                if (ok && part->detail != NULL &&
                    cJSON_AddStringToObject(img, "detail", part->detail) ==
                        NULL) {
                    ok = false;
                }
                if (!ok) {
                    cJSON_Delete(obj);
                    goto fail;
                }
            } else {
                if (cJSON_AddStringToObject(obj, "type", "text") == NULL ||
                    cJSON_AddStringToObject(obj, "text", part->text) == NULL) {
                    cJSON_Delete(obj);
                    goto fail;
                }
            }
            if (cJSON_AddItemToArray(parts, obj) == 0) {
                cJSON_Delete(obj);
                goto fail;
            }
        }
    } else if (msg->content != NULL &&
               cJSON_AddStringToObject(m, "content", msg->content) == NULL) {
        goto fail;
    }
    if (msg->name != NULL &&
        cJSON_AddStringToObject(m, "name", msg->name) == NULL) {
        goto fail;
    }
    if (msg->tool_call_id != NULL &&
        cJSON_AddStringToObject(m, "tool_call_id", msg->tool_call_id) == NULL) {
        goto fail;
    }
    /* assistant-nachricht mit tool-calls zurueckspielen (tool-loop):   */
    if (msg->tool_calls_len > 0) {
        cJSON *calls = cJSON_AddArrayToObject(m, "tool_calls");
        if (calls == NULL) {
            goto fail;
        }
        for (size_t i = 0; i < msg->tool_calls_len; i++) {
            const OaiToolCall *src = &msg->tool_calls[i];
            cJSON *call = cJSON_CreateObject();
            cJSON *function = NULL;
            if (call == NULL ||
                cJSON_AddStringToObject(call, "id", src->id) == NULL ||
                cJSON_AddStringToObject(call, "type", "function") == NULL) {
                cJSON_Delete(call);
                goto fail;
            }
            function = cJSON_AddObjectToObject(call, "function");
            if (function == NULL ||
                cJSON_AddStringToObject(function, "name", src->name) == NULL ||
                cJSON_AddStringToObject(function, "arguments",
                                        src->arguments) == NULL) {
                cJSON_Delete(call);
                goto fail;
            }
            if (cJSON_AddItemToArray(calls, call) == 0) {
                cJSON_Delete(call);
                goto fail;
            }
        }
    }
    return m;

fail:
    cJSON_Delete(m);
    return NULL;
}

/* fehlgeschlagene tool-parameters_json werden als NULL gemeldet,      */
/* damit create() eine verstaendliche fehlermeldung setzen kann        */
static cJSON *params_to_json(const OaiChatCompletionParams *p, bool stream)
{
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return NULL;
    }

    bool failed = true;
    cJSON *messages = NULL;
    cJSON *tools = NULL;

    messages = cJSON_AddArrayToObject(root, "messages");
    if (cJSON_AddStringToObject(root, "model", p->model) == NULL ||
        messages == NULL ||
        cJSON_AddBoolToObject(root, "stream", (cJSON_bool)stream) == NULL) {
        goto out;
    }

    for (size_t i = 0; i < p->messages_len; i++) {
        cJSON *m = message_to_json(&p->messages[i]);
        if (m == NULL || cJSON_AddItemToArray(messages, m) == 0) {
            cJSON_Delete(m);
            goto out;
        }
    }

    if (p->has_temperature &&
        cJSON_AddNumberToObject(root, "temperature", p->temperature) == NULL) {
        goto out;
    }
    if (p->has_top_p &&
        cJSON_AddNumberToObject(root, "top_p", p->top_p) == NULL) {
        goto out;
    }
    if (p->has_n && cJSON_AddNumberToObject(root, "n", p->n) == NULL) {
        goto out;
    }
    if (p->has_max_completion_tokens &&
        cJSON_AddNumberToObject(root, "max_completion_tokens",
                                p->max_completion_tokens) == NULL) {
        goto out;
    }
    if (p->has_max_tokens &&
        cJSON_AddNumberToObject(root, "max_tokens", p->max_tokens) == NULL) {
        goto out;
    }
    if (p->stop_len > 0) {
        cJSON *stop = cJSON_AddArrayToObject(root, "stop");
        if (stop == NULL) {
            goto out;
        }
        for (size_t i = 0; i < p->stop_len; i++) {
            cJSON *s = cJSON_CreateString(p->stop[i]);
            if (s == NULL || cJSON_AddItemToArray(stop, s) == 0) {
                cJSON_Delete(s);
                goto out;
            }
        }
    }
    if (p->has_presence_penalty &&
        cJSON_AddNumberToObject(root, "presence_penalty",
                                p->presence_penalty) == NULL) {
        goto out;
    }
    if (p->has_frequency_penalty &&
        cJSON_AddNumberToObject(root, "frequency_penalty",
                                p->frequency_penalty) == NULL) {
        goto out;
    }

    if (p->has_tool_choice) {
        switch (p->tool_choice) {
        case OAI_TOOL_CHOICE_AUTO:
        case OAI_TOOL_CHOICE_NONE:
        case OAI_TOOL_CHOICE_REQUIRED: {
            const char *choice = "auto";
            if (p->tool_choice == OAI_TOOL_CHOICE_NONE) {
                choice = "none";
            } else if (p->tool_choice == OAI_TOOL_CHOICE_REQUIRED) {
                choice = "required";
            }
            if (cJSON_AddStringToObject(root, "tool_choice", choice) == NULL) {
                goto out;
            }
            break;
        }
        case OAI_TOOL_CHOICE_FUNCTION: {
            cJSON *tc = cJSON_AddObjectToObject(root, "tool_choice");
            cJSON *fn = NULL;
            if (tc != NULL &&
                cJSON_AddStringToObject(tc, "type", "function") != NULL) {
                fn = cJSON_AddObjectToObject(tc, "function");
            }
            if (fn == NULL ||
                cJSON_AddStringToObject(fn, "name", p->tool_choice_function) ==
                    NULL) {
                goto out;
            }
            break;
        }
        }
    }
    if (p->has_parallel_tool_calls &&
        cJSON_AddBoolToObject(root, "parallel_tool_calls",
                              (cJSON_bool)p->parallel_tool_calls) == NULL) {
        goto out;
    }
    if (p->has_seed &&
        cJSON_AddNumberToObject(root, "seed", (double)p->seed) == NULL) {
        goto out;
    }
    if (p->has_response_format) {
        cJSON *rf = cJSON_AddObjectToObject(root, "response_format");
        if (rf == NULL || cJSON_AddStringToObject(
                              rf, "type", p->response_format_type) == NULL) {
            goto out;
        }
        if (p->response_format_json_schema != NULL) {
            cJSON *schema = cJSON_Parse(p->response_format_json_schema);
            if (schema == NULL) {
                goto out;
            }
            if (cJSON_AddItemToObject(rf, "json_schema", schema) == 0) {
                cJSON_Delete(schema);
                goto out;
            }
        }
    }
    if (stream && p->include_usage) {
        cJSON *so = cJSON_AddObjectToObject(root, "stream_options");
        if (so == NULL ||
            cJSON_AddBoolToObject(so, "include_usage", true) == NULL) {
            goto out;
        }
    }

    if (p->tools_len > 0) {
        tools = cJSON_AddArrayToObject(root, "tools");
        if (tools == NULL) {
            goto out;
        }
        for (size_t i = 0; i < p->tools_len; i++) {
            const OaiToolFunction *fn = &p->tools[i].function;
            cJSON *tool = cJSON_CreateObject();
            cJSON *function = NULL;
            if (tool != NULL &&
                cJSON_AddStringToObject(tool, "type", "function") != NULL) {
                function = cJSON_AddObjectToObject(tool, "function");
            }
            if (function == NULL ||
                cJSON_AddStringToObject(function, "name", fn->name) == NULL) {
                cJSON_Delete(tool);
                goto out;
            }
            if (fn->description != NULL &&
                cJSON_AddStringToObject(function, "description",
                                        fn->description) == NULL) {
                cJSON_Delete(tool);
                goto out;
            }
            if (fn->parameters_json != NULL) {
                cJSON *parameters = cJSON_Parse(fn->parameters_json);
                if (parameters == NULL) {
                    cJSON_Delete(tool);
                    goto out;
                }
                if (cJSON_AddItemToObject(function, "parameters", parameters) ==
                    0) {
                    cJSON_Delete(parameters);
                    cJSON_Delete(tool);
                    goto out;
                }
            }
            if (cJSON_AddItemToArray(tools, tool) == 0) {
                cJSON_Delete(tool);
                goto out;
            }
        }
    }

    failed = false;

out:
    if (failed) {
        cJSON_Delete(root);
        return NULL;
    }
    return root;
}

/* ------------------------------------------------------------------ */
/* json-parsing der antworten                                          */
/* ------------------------------------------------------------------ */

static void tool_calls_free(OaiToolCall *tool_calls, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        free(tool_calls[i].id);
        free(tool_calls[i].name);
        free(tool_calls[i].arguments);
    }
    free(tool_calls);
}

/* volle tool-calls (antwort): {id, function: {name, arguments}}       */
static int parse_tool_calls(const cJSON *arr, OaiToolCall **out,
                            size_t *out_len)
{
    *out = NULL;
    *out_len = 0;
    if (!cJSON_IsArray(arr)) {
        return 0;
    }
    size_t n = (size_t)cJSON_GetArraySize(arr);
    if (n == 0) {
        return 0;
    }
    OaiToolCall *calls = calloc(n, sizeof *calls);
    if (calls == NULL) {
        return -1;
    }
    size_t i = 0;
    const cJSON *item;
    cJSON_ArrayForEach(item, arr)
    {
        if (i >= n) {
            break; /* defensiv: mehr items als gezahlt */
        }
        OaiToolCall *call = &calls[i++];
        call->id = get_string_dup(item, "id");
        const cJSON *function =
            cJSON_GetObjectItemCaseSensitive(item, "function");
        if (cJSON_IsObject(function)) {
            call->name = get_string_dup(function, "name");
            call->arguments = get_string_dup(function, "arguments");
        }
    }
    *out = calls;
    *out_len = n;
    return 0;
}

/* content einer nachricht/delta: string ODER (neuere api) array aus    */
/* content-parts. bei arrays werden text-parts konkateniert, so dass     */
/* content am ende immer ein string (oder NULL) ist.                   */
static char *get_content_dup(const cJSON *obj)
{
    const cJSON *content = cJSON_GetObjectItemCaseSensitive(obj, "content");
    if (cJSON_IsString(content) && content->valuestring != NULL) {
        return dup_cstr(content->valuestring);
    }
    if (cJSON_IsArray(content)) {
        Buf b = {0};
        const cJSON *item;
        cJSON_ArrayForEach(item, content)
        {
            if (cJSON_IsString(item) && item->valuestring != NULL) {
                if (buf_append(&b, item->valuestring,
                               strlen(item->valuestring)) != 0) {
                    free(b.buf);
                    return NULL;
                }
            } else {
                const cJSON *text =
                    cJSON_GetObjectItemCaseSensitive(item, "text");
                if (cJSON_IsString(text) && text->valuestring != NULL &&
                    buf_append(&b, text->valuestring,
                               strlen(text->valuestring)) != 0) {
                    free(b.buf);
                    return NULL;
                }
            }
        }
        return b.buf; /* NULL bei leerem array (content: []) */
    }
    return NULL;
}

/* tool-call-deltas (stream): {index, id, function: {name, arguments}} */
static int parse_chunk_tool_calls(const cJSON *arr, OaiChunkToolCall **out,
                                  size_t *out_len)
{
    *out = NULL;
    *out_len = 0;
    if (!cJSON_IsArray(arr)) {
        return 0;
    }
    size_t n = (size_t)cJSON_GetArraySize(arr);
    if (n == 0) {
        return 0;
    }
    OaiChunkToolCall *calls = calloc(n, sizeof *calls);
    if (calls == NULL) {
        return -1;
    }
    size_t i = 0;
    const cJSON *item;
    cJSON_ArrayForEach(item, arr)
    {
        if (i >= n) {
            break; /* defensiv: mehr items als gezahlt */
        }
        OaiChunkToolCall *call = &calls[i++];
        call->index = (size_t)get_int(item, "index", 0);
        call->id = get_string_dup(item, "id");
        const cJSON *function =
            cJSON_GetObjectItemCaseSensitive(item, "function");
        if (cJSON_IsObject(function)) {
            call->name = get_string_dup(function, "name");
            call->arguments_delta = get_string_dup(function, "arguments");
        }
    }
    *out = calls;
    *out_len = n;
    return 0;
}

static int parse_chunk(const cJSON *root, OaiChatCompletionChunk *out)
{
    memset(out, 0, sizeof *out);
    out->id = get_string_dup(root, "id");
    out->model = get_string_dup(root, "model");

    /* usage-chunk (stream_options.include_usage): letzter chunk mit     */
    /* token-zaehlung und leerem choices-array                            */
    const cJSON *usage = cJSON_GetObjectItemCaseSensitive(root, "usage");
    if (cJSON_IsObject(usage)) {
        out->has_usage = true;
        out->usage.prompt_tokens = get_int(usage, "prompt_tokens", 0);
        out->usage.completion_tokens = get_int(usage, "completion_tokens", 0);
        out->usage.total_tokens = get_int(usage, "total_tokens", 0);
    }

    const cJSON *choices = cJSON_GetObjectItemCaseSensitive(root, "choices");
    if (!cJSON_IsArray(choices)) {
        return 0; /* tolerieren: manche chunks haben keine choices */
    }
    size_t n = (size_t)cJSON_GetArraySize(choices);
    if (n > 0) {
        out->choices = calloc(n, sizeof *out->choices);
        if (out->choices == NULL) {
            return -1;
        }
    }
    out->choices_len = n;

    size_t i = 0;
    const cJSON *item;
    cJSON_ArrayForEach(item, choices)
    {
        if (i >= n) {
            break; /* defensiv: mehr items als gezahlt */
        }
        OaiChunkChoice *choice = &out->choices[i];
        i++;
        int idx = get_int(item, "index", 0);
        choice->index = (size_t)(idx > 0 ? idx : 0);
        choice->finish_reason = get_string_dup(item, "finish_reason");

        const cJSON *delta = cJSON_GetObjectItemCaseSensitive(item, "delta");
        if (cJSON_IsObject(delta)) {
            choice->role = get_string_dup(delta, "role");
            choice->content_delta = get_content_dup(delta);
            if (parse_chunk_tool_calls(
                    cJSON_GetObjectItemCaseSensitive(delta, "tool_calls"),
                    &choice->tool_call_deltas,
                    &choice->tool_call_deltas_len) != 0) {
                return -1;
            }
        }
    }
    return 0;
}

static int parse_completion(const cJSON *root, OaiChatCompletion *out)
{
    memset(out, 0, sizeof *out);
    out->id = get_string_dup(root, "id");
    out->model = get_string_dup(root, "model");

    const cJSON *choices = cJSON_GetObjectItemCaseSensitive(root, "choices");
    if (!cJSON_IsArray(choices)) {
        return -1;
    }
    size_t n = (size_t)cJSON_GetArraySize(choices);
    if (n > 0) {
        out->choices = calloc(n, sizeof *out->choices);
        if (out->choices == NULL) {
            return -1;
        }
    }
    out->choices_len = n;

    size_t i = 0;
    const cJSON *item;
    cJSON_ArrayForEach(item, choices)
    {
        if (i >= n) {
            break; /* defensiv: mehr items als gezahlt */
        }
        OaiChoice *choice = &out->choices[i];
        i++;
        int idx = get_int(item, "index", 0);
        choice->index = (size_t)(idx > 0 ? idx : 0);
        choice->finish_reason = get_string_dup(item, "finish_reason");

        const cJSON *msg = cJSON_GetObjectItemCaseSensitive(item, "message");
        if (cJSON_IsObject(msg)) {
            choice->message.role = get_string_dup(msg, "role");
            choice->message.content = get_content_dup(msg);
            if (parse_tool_calls(
                    cJSON_GetObjectItemCaseSensitive(msg, "tool_calls"),
                    &choice->message.tool_calls,
                    &choice->message.tool_calls_len) != 0) {
                return -1;
            }
        }
    }

    const cJSON *usage = cJSON_GetObjectItemCaseSensitive(root, "usage");
    if (cJSON_IsObject(usage)) {
        out->has_usage = true;
        out->usage.prompt_tokens = get_int(usage, "prompt_tokens", 0);
        out->usage.completion_tokens = get_int(usage, "completion_tokens", 0);
        out->usage.total_tokens = get_int(usage, "total_tokens", 0);
    }
    return 0;
}

/* fehlermeldung aus einem api-fehler-koerper holen:                   */
/* {"error": {"message": "...", ...}}                                  */
static char *extract_error_message(const char *body)
{
    cJSON *root = cJSON_Parse(body);
    if (root == NULL) {
        return NULL;
    }
    const cJSON *err = cJSON_GetObjectItemCaseSensitive(root, "error");
    char *msg = NULL;
    if (cJSON_IsObject(err)) {
        msg = get_string_dup(err, "message");
    } else if (cJSON_IsString(err)) {
        msg = dup_cstr(err->valuestring);
    }
    cJSON_Delete(root);
    return msg;
}

/* ------------------------------------------------------------------ */
/* free-funktionen                                                     */
/* ------------------------------------------------------------------ */

void oai_chat_completion_free(OaiChatCompletion *completion)
{
    if (completion == NULL) {
        return;
    }
    free(completion->id);
    free(completion->model);
    for (size_t i = 0; i < completion->choices_len; i++) {
        OaiChoice *choice = &completion->choices[i];
        free(choice->message.role);
        free(choice->message.content);
        tool_calls_free(choice->message.tool_calls,
                        choice->message.tool_calls_len);
        free(choice->finish_reason);
    }
    free(completion->choices);
    memset(completion, 0, sizeof *completion);
}

void oai_chat_completion_chunk_free(OaiChatCompletionChunk *chunk)
{
    if (chunk == NULL) {
        return;
    }
    free(chunk->id);
    free(chunk->model);
    for (size_t i = 0; i < chunk->choices_len; i++) {
        OaiChunkChoice *choice = &chunk->choices[i];
        free(choice->role);
        free(choice->content_delta);
        for (size_t j = 0; j < choice->tool_call_deltas_len; j++) {
            free(choice->tool_call_deltas[j].id);
            free(choice->tool_call_deltas[j].name);
            free(choice->tool_call_deltas[j].arguments_delta);
        }
        free(choice->tool_call_deltas);
        free(choice->finish_reason);
    }
    free(chunk->choices);
    memset(chunk, 0, sizeof *chunk);
}

void oai_completion_result_free(OaiCompletionResult *result)
{
    if (result == NULL) {
        return;
    }
    free(result->error);
    oai_chat_completion_free(&result->completion);
    memset(result, 0, sizeof *result);
}

/* ------------------------------------------------------------------ */
/* sse-parsing                                                         */
/* ------------------------------------------------------------------ */

/* ein komplettes sse-event (durch leerzeile getrennt) verarbeiten.    */
/* zeilenenden sind \n oder \r\n (sse-spec), trailing \r wird hier     */
/* entfernt. mehrere data-zeilen pro event werden laut spec mit \n     */
/* verbunden; "event:"/"id:"-zeilen und kommentare (":...") werden    */
/* ignoriert.                                                          */
static void sse_handle_event(RequestState *st, const char *event)
{
    Buf data = {0};
    bool done = false;

    const char *line = event;
    while (line != NULL && *line != '\0') {
        const char *end = strchr(line, '\n');
        size_t line_len = end != NULL ? (size_t)(end - line) : strlen(line);

        size_t content_len = line_len;
        while (content_len > 0 && line[content_len - 1] == '\r') {
            content_len--; /* crlf-zeilenende normalisieren */
        }

        if (content_len > 5 && strncmp(line, "data:", 5) == 0) {
            const char *payload = line + 5;
            size_t payload_len = content_len - 5;
            while (payload_len > 0 && *payload == ' ') {
                payload++;
                payload_len--;
            }
            /* trailing \r ist schon entfernt; spaces am zeilenende sind    */
            /* fuer [DONE]-erkennung und json-payloads bedeutungslos         */
            while (payload_len > 0 && payload[payload_len - 1] == ' ') {
                payload_len--;
            }
            if (payload_len == 6 && strncmp(payload, "[DONE]", 6) == 0) {
                done = true;
            } else if (payload_len > 0) {
                if (buf_append(&data, "\n", 1) != 0 ||
                    buf_append(&data, payload, payload_len) != 0) {
                    /* out of memory: event verwerfen, weiterlesen */
                    data.len = 0;
                }
            }
        }
        line = end != NULL ? end + 1 : NULL;
    }

    if (done) {
        free(data.buf);
        st->sse_done = true;
        return;
    }

    if (data.buf != NULL && data.len > 0) {
        cJSON *root = cJSON_Parse(data.buf);
        if (root != NULL) {
            OaiChatCompletionChunk chunk;
            int parse_rc = parse_chunk(root, &chunk);
            cJSON_Delete(root);
            if (parse_rc == 0) {
                st->chunks_delivered++;
                if (st->cb.on_chunk != NULL &&
                    st->cb.on_chunk(&chunk, st->cb.user_data) != 0) {
                    st->aborted = true;
                }
            }
            /* unparsebares event: ignorieren, nicht abbrechen */
            oai_chat_completion_chunk_free(&chunk);
        }
    }
    free(data.buf);

    if (st->aborted) {
        return;
    }
}

/* alle komplett uebertragenen sse-events im puffer verarbeiten. ein  */
/* event-ende ist eine leerzeile: "\n\n" oder "\r\n\r\n" (sse-spec).  */
static void sse_process(RequestState *st)
{
    char *buf = st->body->buf;
    size_t len = st->body->len;

    while (!st->sse_done && !st->aborted && st->sse_pos < len) {
        size_t i = st->sse_pos;
        size_t sep = 0;
        while (i < len) {
            if (i + 1 < len && buf[i] == '\n' && buf[i + 1] == '\n') {
                sep = 2;
                break;
            }
            if (i + 3 < len && buf[i] == '\r' && buf[i + 1] == '\n' &&
                buf[i + 2] == '\r' && buf[i + 3] == '\n') {
                sep = 4;
                break;
            }
            i++;
        }
        if (sep == 0) {
            break; /* noch kein komplettes event */
        }
        buf[i] = '\0'; /* event abschneiden (inhalt ist konsumiert) */
        sse_handle_event(st, buf + st->sse_pos);
        st->sse_pos = i + sep;
    }
}

/* ------------------------------------------------------------------ */
/* http                                                                */
/* ------------------------------------------------------------------ */

/* fehlermeldung in einen heap-string setzen (printf-format) */
static void result_set_error(OaiCompletionResult *result, long http_status,
                             const char *fmt, ...)
{
    char stack[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(stack, sizeof stack, fmt, args);
    va_end(args);
    result->ok = false;
    result->http_status = http_status;
    result->error = dup_cstr(stack);
    if (result->error == NULL) {
        result->error = dup_cstr("out of memory");
    }
}

static size_t write_cb(char *ptr, size_t size, size_t nmemb, void *ud)
{
    RequestState *st = ud;
    size_t total = size * nmemb;

    /* body-deckel: schuetzt vor OOM durch fehlleitende/robuste server */
    long cap = st->client->max_body_bytes;
    if (cap > 0 && st->body->len + total > (size_t)cap) {
        st->overflow = true;
        return 0;
    }
    if (buf_append(st->body, ptr, total) != 0) {
        return 0; /* speicherfehler -> curl bricht die uebertragung ab */
    }
    st->last_activity = mono_now();
    if (st->stream) {
        sse_process(st);
    }
    if (st->aborted) {
        return 0; /* callback wollte abbrechen */
    }
    return total;
}

/* response-header nach retry-after / retry-after-ms durchsuchen, damit */
/* das backoff dem server-wunsch folgt (wie im npm-package). letzte      */
/* zeile gewinnt (bei mehrfacher uebermittlung, z.B. 1xx + final).        */
static size_t header_cb(char *ptr, size_t size, size_t nmemb, void *ud)
{
    RequestState *st = ud;
    size_t total = size * nmemb;

    const char *val = NULL;
    size_t vlen = 0;
    if (total >= 16 && strncasecmp(ptr, "retry-after-ms:", 16) == 0) {
        val = ptr + 16;
        vlen = total - 16;
    } else if (total >= 12 && strncasecmp(ptr, "retry-after:", 12) == 0) {
        val = ptr + 12;
        vlen = total - 12;
    }
    if (val == NULL) {
        return total;
    }
    while (vlen > 0 && (*val == ' ' || *val == '\t')) {
        val++;
        vlen--;
    }
    while (vlen > 0 && (val[vlen - 1] == '\r' || val[vlen - 1] == '\n' ||
                        val[vlen - 1] == ' ' || val[vlen - 1] == '\t')) {
        vlen--;
    }
    if (vlen == 0 || vlen >= sizeof st->retry_after) {
        return total;
    }
    memcpy(st->retry_after, val, vlen);
    st->retry_after[vlen] = '\0';
    st->last_activity = mono_now();
    return total;
}

/* idle-watchdog: ruft curl regelmaessig auf (~100ms), auch wenn der  */
/* socket stille ist. rueckgabe != 0 bricht die uebertragung ab      */
/* (CURLE_ABORTED_BY_CALLBACK). genauer als CURLOPT_LOW_SPEED_*     */
/* (das erst nach ~6s zusaetzlich reagiert).                        */
static int xferinfo_cb(void *ud, curl_off_t dltotal, curl_off_t dlnow,
                       curl_off_t ultotal, curl_off_t ulnow)
{
    (void)dltotal;
    (void)dlnow;
    (void)ultotal;
    (void)ulnow;
    RequestState *st = ud;
    /* abbruch von aussen hat vorrang: er ist kein fehler, der stream
     * endet wie nach einem abbruch aus on_chunk */
    if (st->cb.should_abort != NULL &&
        st->cb.should_abort(st->cb.user_data) != 0) {
        st->aborted = true;
        return 1;
    }
    if (st->client->stream_idle_timeout_ms > 0 &&
        mono_now() - st->last_activity >
            (double)st->client->stream_idle_timeout_ms / 1000.0) {
        st->idle_hit = true;
        return 1;
    }
    return 0;
}

/* eine einzelne http-anfrage. alle ergebnisse (erfolg und fehler)     */
/* landen in result; der rueckgabewert ist nur fuer abbruch-relevant.  */
/* clang-tidy: die typecheck-makros von curl_easy_setopt und cJSON
 * zaehlen als tausende statements; die funktion ist in wirklichkeit
 * linear und einfach zu lesen. */
static void perform_once( // NOLINT(readability-function-size)
    RequestState *st)
{
    OaiCompletionResult *result = st->result;
    Buf body = {0};
    st->body = &body;
    st->sse_pos = 0;
    st->sse_done = false;
    st->aborted = false;
    st->no_retry = false;
    st->overflow = false;
    st->idle_hit = false;
    st->last_activity = mono_now();
    st->retry_after[0] = '\0';

    char *json = NULL;
    char *url = NULL;
    char *auth = NULL;
    struct curl_slist *headers = NULL;
    char errbuf[CURL_ERROR_SIZE] = {0};

    CURLcode cret = CURLE_OK;
    long status = 0;
    CURL *curl = NULL;
    bool owned = false;

    cJSON *payload = params_to_json(st->params, st->stream);
    if (payload == NULL) {
        /* deterministisch (invalides schema oder oom): retry unnoetig */
        st->no_retry = true;
        result_set_error(result, 0,
                         "anfrage konnte nicht aufgebaut werden "
                         "(out of memory, ungueltiges tool-schema oder "
                         "response-format)");
        goto out;
    }
    json = cJSON_PrintUnformatted(payload);
    cJSON_Delete(payload);
    if (json == NULL) {
        st->no_retry = true;
        result_set_error(result, 0, "out of memory beim json-aufbau");
        goto out;
    }

    /* url: base_url (ohne trailing slash) + "/chat/completions"       */
    {
        size_t blen = strlen(st->client->base_url);
        const char *path = "/chat/completions";
        size_t plen = strlen(path);
        url = malloc(blen + plen + 1);
        if (url == NULL) {
            result_set_error(result, 0, "out of memory");
            goto out;
        }
        memcpy(url, st->client->base_url, blen);
        memcpy(url + blen, path, plen + 1);
    }

    /* handle wiederverwenden: curl_easy_reset setzt alle optionen     */
    /* zurueck, der verbindungs-cache (und damit keep-alive + tls-     */
    /* session-wiederverwendung) bleibt erhalten. performance-relevant */
    /* fuer chat-clients: ohne reuse wuerde jede nachricht einen       */
    /* tls-handshake kosten.                                           */
    curl = (CURL *)st->client->curl_handle;
    if (curl == NULL) {
        curl = curl_easy_init();
        owned = true;
    } else {
        curl_easy_reset(curl);
    }
    if (curl == NULL) {
        result_set_error(result, 0, "curl_easy_init fehlgeschlagen");
        goto out;
    }

    {
        size_t auth_len = strlen(st->client->api_key) + 24;
        auth = malloc(auth_len);
        if (auth == NULL) {
            result_set_error(result, 0, "out of memory");
            goto out;
        }
        snprintf(auth, auth_len, "Authorization: Bearer %s",
                 st->client->api_key);
    }
    /* append-chain ohne leck und ohne stummen verlust einzelner header: */
    /* `headers` bleibt immer auf der letzten gueltigen liste (wird bei  */
    /* out: gefreet), `tmp` rollt voran; schlaegt ein append fehl, wird */
    /* die ganze anfrage abgebrochen.                                   */
    {
        struct curl_slist *tmp = NULL;
        headers = curl_slist_append(NULL, "Content-Type: application/json");
        if (headers != NULL) {
            tmp = curl_slist_append(headers, "Accept: application/json");
        }
        if (tmp != NULL) {
            tmp = curl_slist_append(tmp, auth);
        }
        if (tmp == NULL) {
            result_set_error(result, 0, "out of memory bei headers");
            goto out;
        }
        headers = tmp;
    }

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, OAI_USER_AGENT);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, ""); /* gzip/deflate */
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, st);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_cb);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, st);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, errbuf);
    if (st->stream) {
        /* gesamt-timeout wuerde lange streams abwuergen; deshalb nur  */
        /* ein connect-timeout. gegen haengende streams gibt es einen   */
        /* idle-watchdog: xferinfo_cb wird auch bei stillem socket      */
        /* regelmaessig gerufen und bricht bei datenstillstand ab      */
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS,
                         OAI_STREAM_CONNECT_TIMEOUT_MS);
        /* der watchdog traegt zwei aufgaben: idle-timeout UND den
         * abbruch von aussen. einer von beiden genuegt, damit er
         * gebraucht wird. */
        if (st->client->stream_idle_timeout_ms > 0 ||
            st->cb.should_abort != NULL) {
            curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
            curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, xferinfo_cb);
            curl_easy_setopt(curl, CURLOPT_XFERINFODATA, st);
        }
    } else {
        curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, st->client->timeout_ms);
    }

    cret = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    result->http_status = status;

    if (st->aborted) {
        /* abbruch vom on_chunk-callback: wie ein sauberes ende       */
        /* behandeln (entspricht break im async-iterator)             */
        result->ok = true;
        goto out;
    }

    if (st->overflow) {
        st->no_retry = true; /* gleiches ergebnis beim retry */
        result_set_error(result, 0,
                         "antwort zu gross: limit von %ld bytes "
                         "ueberschritten",
                         st->client->max_body_bytes);
        goto out;
    }

    if (cret != CURLE_OK) {
        if (st->idle_hit) {
            result_set_error(
                result, 0,
                "stream-idle-timeout: seit %ld ms keine daten mehr "
                "empfangen",
                st->client->stream_idle_timeout_ms);
            goto out;
        }
        const char *detail =
            errbuf[0] != '\0' ? errbuf : curl_easy_strerror(cret);
        result_set_error(result, 0, "verbindungsfehler: %s", detail);
        goto out;
    }

    if (status >= 200 && status < 300) {
        if (st->stream) {
            /* ohne [DONE] ist die antwort unvollstaendig (npn-abbruch, */
            /* gateway-timeout, ...) -> als fehler melden statt still   */
            /* halbe antworten durchzulassen                            */
            if (st->sse_done) {
                result->ok = true;
            } else {
                result_set_error(result, status,
                                 "stream endete vorzeitig (kein [DONE])");
            }
            goto out;
        }
        cJSON *root = cJSON_Parse(body.buf != NULL ? body.buf : "");
        if (root == NULL) {
            result_set_error(result, status,
                             "ungueltige json-antwort vom server");
            goto out;
        }
        int parse_rc = parse_completion(root, &result->completion);
        cJSON_Delete(root);
        if (parse_rc != 0) {
            result_set_error(result, status,
                             "antwort konnte nicht geparsed werden");
            goto out;
        }
        result->ok = true;
        goto out;
    }

    /* http-fehler: fehlermeldung aus dem body extrahieren             */
    {
        char *msg = extract_error_message(body.buf != NULL ? body.buf : "");
        if (msg != NULL) {
            result_set_error(result, status, "%s", msg);
            free(msg);
        } else {
            result_set_error(result, status, "http-fehler %ld: %s", status,
                             body.buf != NULL ? body.buf : "(leer)");
        }
        goto out;
    }

out:
    curl_slist_free_all(headers);
    if (owned) {
        curl_easy_cleanup(curl);
    }
    free(auth);
    free(url);
    free(json);
    free(body.buf);
}

static bool http_retryable(long status)
{
    /* npm shouldRetry: 408 (request timeout), 409 (lock timeout),      */
    /* 429 (rate limit), >= 500 (internal errors)                        */
    if (status == 408 || status == 409 || status == 429 || status >= 500) {
        return true;
    }
    return false;
}

/* retry-schleife um perform_once: npm-default maxRetries = 2,         */
/* exponential backoff; nur bei verbindungsfehlern, 408, 429 und 5xx.  */
/* bei streaming wird nicht wiederholt, wenn schon chunks zugestellt   */
/* wurden (die daten sind dann schon beim nutzer).                     */
static void perform_with_retries(RequestState *st)
{
    for (int attempt = 0;; attempt++) {
        perform_once(st);

        if (st->aborted || st->result->ok) {
            return;
        }
        long status = st->result->http_status;
        bool retryable = status == 0;
        if (!retryable) {
            retryable = http_retryable(status);
        }
        if (!retryable || st->no_retry || attempt >= st->client->max_retries ||
            (st->stream && st->chunks_delivered > 0)) {
            return;
        }

        oai_completion_result_free(st->result);
        memset(st->result, 0, sizeof *st->result);
        backoff_sleep(attempt, st->retry_after);
    }
}

/* ------------------------------------------------------------------ */
/* oeffentliche api                                                    */
/* ------------------------------------------------------------------ */

int oai_client_init(OaiClient *client, const OaiClientOptions *options)
{
    if (client == NULL || options == NULL || options->api_key == NULL) {
        return -1;
    }
    memset(client, 0, sizeof *client);
    if (curl_ensure_global() != 0) {
        return -1;
    }

    client->api_key = dup_cstr(options->api_key);
    if (client->api_key == NULL) {
        curl_release_global();
        return -1;
    }
    if (options->base_url != NULL) {
        /* trailing slashes abschneiden */
        size_t len = strlen(options->base_url);
        while (len > 0 && options->base_url[len - 1] == '/') {
            len--;
        }
        client->base_url = malloc(len + 1);
        if (client->base_url == NULL) {
            oai_client_free(client);
            return -1;
        }
        memcpy(client->base_url, options->base_url, len);
        client->base_url[len] = '\0';
    } else {
        client->base_url = dup_cstr(OAI_DEFAULT_BASE_URL);
        if (client->base_url == NULL) {
            oai_client_free(client);
            return -1;
        }
    }
    client->curl_handle = curl_easy_init();
    client->timeout_ms =
        options->timeout_ms > 0 ? options->timeout_ms : OAI_DEFAULT_TIMEOUT_MS;
    client->max_retries = options->max_retries >= 0 ? options->max_retries
                                                    : OAI_DEFAULT_MAX_RETRIES;
    client->stream_idle_timeout_ms = options->stream_idle_timeout_ms;
    if (client->stream_idle_timeout_ms == 0) {
        client->stream_idle_timeout_ms = OAI_DEFAULT_STREAM_IDLE_MS;
    }
    client->max_body_bytes = options->max_body_bytes;
    if (client->max_body_bytes == 0) {
        client->max_body_bytes = OAI_DEFAULT_MAX_BODY_BYTES;
    }
    return 0;
}

void oai_client_free(OaiClient *client)
{
    if (client == NULL) {
        return;
    }
    curl_easy_cleanup((CURL *)client->curl_handle);
    client->curl_handle = NULL;
    free(client->api_key);
    free(client->base_url);
    memset(client, 0, sizeof *client);
    curl_release_global();
}

int oai_chat_completions_create(const OaiClient *client,
                                const OaiChatCompletionParams *params,
                                OaiCompletionResult *result)
{
    if (client == NULL || params == NULL || result == NULL ||
        params_validate(params) != 0) {
        return -1;
    }
    memset(result, 0, sizeof *result);
    RequestState st = {
        .client = client,
        .params = params,
        .stream = false,
        .result = result,
    };
    perform_with_retries(&st);
    return 0;
}

int oai_chat_completions_create_stream(const OaiClient *client,
                                       const OaiChatCompletionParams *params,
                                       const OaiStreamCallbacks *callbacks)
{
    if (client == NULL || params == NULL || callbacks == NULL ||
        params_validate(params) != 0) {
        if (callbacks != NULL && callbacks->on_error != NULL) {
            callbacks->on_error(0, "ungueltige parameter (NULL-felder)",
                                callbacks->user_data);
        }
        return -1;
    }

    /* tools frueh validieren, damit ein kaputtes parameters_json nicht */
    /* erst beim streamen auffaellt                                     */
    for (size_t i = 0; i < params->tools_len; i++) {
        const char *schema = params->tools[i].function.parameters_json;
        if (schema != NULL) {
            cJSON *test = cJSON_Parse(schema);
            if (test == NULL) {
                if (callbacks->on_error != NULL) {
                    callbacks->on_error(0,
                                        "ungueltiges parameters_json bei "
                                        "einem tool",
                                        callbacks->user_data);
                }
                return -1;
            }
            cJSON_Delete(test);
        }
    }

    /* gleiches fruehwarnsystem fuer das response-format-schema        */
    if (params->has_response_format &&
        params->response_format_json_schema != NULL) {
        cJSON *test = cJSON_Parse(params->response_format_json_schema);
        if (test == NULL) {
            if (callbacks->on_error != NULL) {
                callbacks->on_error(0,
                                    "ungueltiges response_format_json_schema",
                                    callbacks->user_data);
            }
            return -1;
        }
        cJSON_Delete(test);
    }

    OaiCompletionResult result;
    memset(&result, 0, sizeof result);
    RequestState st = {
        .client = client,
        .params = params,
        .stream = true,
        .cb = *callbacks,
        .result = &result,
    };
    perform_with_retries(&st);

    if (!result.ok && callbacks->on_error != NULL) {
        callbacks->on_error(result.http_status,
                            result.error != NULL ? result.error
                                                 : "unbekannter fehler",
                            callbacks->user_data);
    }
    oai_completion_result_free(&result);
    return 0;
}