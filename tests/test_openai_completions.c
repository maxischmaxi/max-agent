// tests fuer openai_completions gegen einen lokalen mock-server
// (fork + raw sockets, kein netzwerk-zugriff noetig).
//
// der server-child erbt einen listening-socket und antwortet je nach
// "model"-feld im request-body: normal, mit tool-calls, mit http-401,
// mit kaputtem json, mit sse-stream, mit vorzeitig geschlossenem
// stream usw. der parent testet die komplette lib dagegen.
#define _POSIX_C_SOURCE 200809L

#include <arpa/inet.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "openai_completions.h"
#include "test.h"

#define REQ_MAX 16384

/* ------------------------------------------------------------------ */
/* minimaler http-server im child-prozess                              */
/* ------------------------------------------------------------------ */

static int read_request(int fd, char *buf, size_t cap, char **body)
{
    size_t total = 0;
    char *hdr_end = NULL;
    buf[0] = '\0';
    while (hdr_end == NULL && total < cap - 1) {
        ssize_t n = read(fd, buf + total, cap - 1 - total);
        if (n <= 0) {
            return -1;
        }
        total += (size_t)n;
        buf[total] = '\0';
        hdr_end = strstr(buf, "\r\n\r\n");
    }
    if (hdr_end == NULL) {
        return -1;
    }

    long content_length = 0;
    const char *cl = strstr(buf, "Content-Length:");
    if (cl != NULL) {
        content_length = strtol(cl + 15, NULL, 10);
    }
    size_t have = total - (size_t)(hdr_end + 4 - buf);
    while (have < (size_t)content_length && total < cap - 1) {
        ssize_t n = read(fd, buf + total, cap - 1 - total);
        if (n <= 0) {
            return -1;
        }
        total += (size_t)n;
        buf[total] = '\0';
        have += (size_t)n;
    }
    *body = hdr_end + 4;
    return 0;
}

/* antwort je nach "model" im body routen */
/* antwort mit korrekt berechneter content-length senden (hardcoded
 * laengen wuerden zu haengenden requests fuehren) */
static const char *default_json(void)
{
    return "{\"id\":\"c1\",\"model\":\"test\",\"choices\":[{\"index\":0,"
           "\"message\":{\"role\":\"assistant\",\"content\":\"antwort\"},"
           "\"finish_reason\":\"stop\"}],\"usage\":{\"prompt_tokens\":5,"
           "\"completion_tokens\":2,\"total_tokens\":7}}";
}

static void send_json(int fd, const char *status, const char *json)
{
    char head[256];
    int head_len = snprintf(head, sizeof head,
                            "HTTP/1.1 %s\r\n"
                            "Content-Type: application/json\r\n"
                            "Content-Length: %zu\r\n\r\n",
                            status, strlen(json));
    if (head_len < 0) {
        return;
    }
    write(fd, head, (size_t)head_len);
    write(fd, json, strlen(json));
}

static void send_json_headers(int fd, const char *status, const char *extra,
                              const char *json)
{
    char head[512];
    int head_len = snprintf(head, sizeof head,
                            "HTTP/1.1 %s\r\n"
                            "%s"
                            "Content-Type: application/json\r\n"
                            "Content-Length: %zu\r\n\r\n",
                            status, extra, strlen(json));
    if (head_len < 0) {
        return;
    }
    write(fd, head, (size_t)head_len);
    write(fd, json, strlen(json));
}

static void sleep_ms(long ms)
{
    struct timespec ts = {
        .tv_sec = ms / 1000,
        .tv_nsec = (ms % 1000) * 1000000L,
    };
    nanosleep(&ts, NULL);
}

/* index des ersten fehlenden substrings im request, -1 wenn alle da */
static int first_missing(const char *body, const char *const *need, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (strstr(body, need[i]) == NULL) {
            return (int)i;
        }
    }
    return -1;
}

static void send_missing(int fd, int idx)
{
    char msg[128];
    int ml = snprintf(msg, sizeof msg,
                      "{\"error\":{\"message\":\"erwartetes request-"
                      "feld %d fehlt\"}}",
                      idx);
    if (ml > 0) {
        send_json(fd, "400 Bad Request", msg);
    }
}

static void handle_conn(int fd)
{
    static char req[REQ_MAX];
    char *body = NULL;
    for (;;) { /* keep-alive: mehrere requests pro verbindung moeglich */
        if (read_request(fd, req, sizeof req, &body) != 0) {
            return;
        }

        if (strstr(body, "\"model\":\"error\"") != NULL) {
            send_json(fd, "401 Unauthorized",
                      "{\"error\":{\"message\":\"invalid api key\"}}");
            continue;
        }
        if (strstr(body, "\"model\":\"reasoning\"") != NULL) {
            /* nicht-streaming: thinking in reasoning_content, cache-
             * anteil unter prompt_tokens_details (openai-form) */
            send_json(fd, "200 OK",
                      "{\"id\":\"c4\",\"model\":\"reasoning\",\"choices\":[{"
                      "\"index\":0,\"message\":{\"role\":\"assistant\","
                      "\"content\":\"antwort\",\"reasoning_content\":"
                      "\"ich denke\"},\"finish_reason\":\"stop\"}],"
                      "\"usage\":{\"prompt_tokens\":5,\"completion_tokens\":"
                      "2,\"total_tokens\":7,\"prompt_tokens_details\":"
                      "{\"cached_tokens\":4}}}");
            continue;
        }
        if (strstr(body, "\"model\":\"reasonround\"") != NULL) {
            /* roundtrip: assistant-thinking muss als reasoning_content
             * zurueckgespielt werden (agent-loop) */
            static const char *const need[] = {
                "\"role\":\"assistant\"",
                "\"reasoning_content\":\"ich denke\"",
                "\"content\":\"antwort\"",
            };
            int miss = first_missing(body, need, 3);
            if (miss >= 0) {
                send_missing(fd, miss);
                continue;
            }
            send_json(fd, "200 OK", default_json());
            continue;
        }
        if (strstr(body, "\"model\":\"reasonstream\"") != NULL) {
            /* stream: thinking-deltas vor dem content, cache-anteil
             * top-level als cached_tokens (ollama-artig). request
             * muss reasoning_effort enthalten */
            static const char *const need[] = {
                "\"reasoning_effort\":\"high\"",
            };
            int miss = first_missing(body, need, 1);
            if (miss >= 0) {
                send_missing(fd, miss);
                continue;
            }
            const char *resp =
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: text/event-stream\r\n"
                "Connection: close\r\n\r\n"
                "data: {\"id\":\"c1\",\"model\":\"t\",\"choices\":[{"
                "\"index\":0,\"delta\":{\"role\":\"assistant\","
                "\"reasoning_content\":\"denk\"}}]}\n\n"
                "data: {\"id\":\"c1\",\"model\":\"t\",\"choices\":[{"
                "\"index\":0,\"delta\":{\"reasoning_content\":\"e\","
                "\"content\":\"ant\"}}]}\n\n"
                "data: {\"id\":\"c1\",\"model\":\"t\",\"choices\":[{"
                "\"index\":0,\"delta\":{},\"finish_reason\":\"stop\"}]}"
                "\n\n"
                "data: {\"id\":\"c1\",\"model\":\"t\",\"choices\":[],"
                "\"usage\":{\"prompt_tokens\":10,\"completion_tokens\":3,"
                "\"total_tokens\":13,\"cached_tokens\":6}}\n\n"
                "data: [DONE]\n\n";
            write(fd, resp, strlen(resp));
            return;
        }
        if (strstr(body, "\"model\":\"malformed\"") != NULL) {
            send_json(fd, "200 OK", "{kein json!");
            continue;
        }
        if (strstr(body, "\"model\":\"flaky\"") != NULL) {
            /* erster request 500, danach ok -> testet das retry */
            static int flaky_calls = 0;
            flaky_calls++;
            if (flaky_calls == 1) {
                send_json(fd, "500 Internal Server Error", "{}\n");
            } else {
                send_json(fd, "200 OK", default_json());
            }
            continue;
        }
        if (strstr(body, "\"model\":\"conflict\"") != NULL) {
            /* erster request 409 (npm: retryable), danach ok */
            static int conflict_calls = 0;
            conflict_calls++;
            if (conflict_calls == 1) {
                send_json(fd, "409 Conflict", "{}");
            } else {
                send_json(fd, "200 OK", default_json());
            }
            continue;
        }
        if (strstr(body, "\"model\":\"rate\"") != NULL) {
            /* erster request 429 mit retry-after: 0, danach ok -> testet */
            /* das parsen des retry-after-headers */
            static int rate_calls = 0;
            rate_calls++;
            if (rate_calls == 1) {
                send_json_headers(fd, "429 Too Many Requests",
                                  "Retry-After: 0\r\n",
                                  "{\"error\":{\"message\":\"rate limited\"}}");
            } else {
                send_json(fd, "200 OK", default_json());
            }
            continue;
        }
        if (strstr(body, "\"model\":\"slowstream\"") != NULL) {
            /* sse in einzelnen tcp-paketen (wie echte server): ein event */
            /* pro write mit pause dazwischen */
            const char *head = "HTTP/1.1 200 OK\r\n"
                               "Content-Type: text/event-stream\r\n"
                               "Connection: close\r\n\r\n";
            write(fd, head, strlen(head));
            const char *e1 = "data: {\"id\":\"c1\",\"model\":\"t\",\"choices\":"
                             "[{\"index\":0,\"delta\":{\"role\":\"assistant\","
                             "\"content\":\"hal\"}}]}\n\n";
            const char *e2 =
                "data: {\"id\":\"c1\",\"model\":\"t\",\"choices\":"
                "[{\"index\":0,\"delta\":{\"content\":\"lo\"}}]"
                "}\r\n"; /* trenner \"\n\" erst mit dem naechsten write */
            const char *e3 = "\n"
                             "data: {\"id\":\"c1\",\"model\":\"t\",\"choices\":"
                             "[{\"index\":0,\"delta\":{},\"finish_reason\":"
                             "\"stop\"}]}\n\n"
                             "data: [DONE] \n\n"; /* trailing-space-[DONE] */
            write(fd, e1, strlen(e1));
            sleep_ms(30);
            write(fd, e2, strlen(e2));
            sleep_ms(30);
            write(fd, e3, strlen(e3));
            return;
        }
        if (strstr(body, "\"model\":\"full\"") != NULL) {
            /* alle neuen request-felder muessen korrekt serialisiert sein */
            static const char *const need[] = {
                "\"content\":[{\"type\":\"text\",\"text\":\"hi\"},{\"type\":"
                "\"image_url\",\"image_url\":{\"url\":\"data:image/png;"
                "base64,QUJD\"}}]",
                "\"tool_choice\":\"auto\"",
                "\"parallel_tool_calls\":false",
                "\"seed\":42",
                "\"response_format\":{\"type\":\"json_object\"}",
            };
            int miss = first_missing(body, need, 5);
            if (miss >= 0) {
                send_missing(fd, miss);
                continue;
            }
            send_json(fd, "200 OK", default_json());
            continue;
        }
        if (strstr(body, "\"model\":\"fullstream\"") != NULL) {
            /* tool_choice named + stream_options.include_usage im request */
            static const char *const need[] = {
                "\"tool_choice\":{\"type\":\"function\",\"function\":"
                "{\"name\":\"get_weather\"}}",
                "\"stream_options\":{\"include_usage\":true}",
                "\"tools\":[{\"type\":\"function\",\"function\":"
                "{\"name\":\"get_weather\"",
            };
            int miss = first_missing(body, need, 3);
            if (miss >= 0) {
                send_missing(fd, miss);
                continue;
            }
            const char *resp =
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: text/event-stream\r\n"
                "Connection: close\r\n\r\n"
                "data: {\"id\":\"c1\",\"model\":\"t\",\"choices\":[{"
                "\"index\":0,\"delta\":{\"role\":\"assistant\","
                "\"content\":\"hal\"}}]}\n\n"
                "data: {\"id\":\"c1\",\"model\":\"t\",\"choices\":[{"
                "\"index\":0,\"delta\":{},\"finish_reason\":\"stop\"}]}"
                "\n\n"
                "data: {\"id\":\"c1\",\"model\":\"t\",\"choices\":[],"
                "\"usage\":{\"prompt_tokens\":5,\"completion_tokens\":7,"
                "\"total_tokens\":12}}\n\n"
                "data: [DONE]\n\n";
            write(fd, resp, strlen(resp));
            return;
        }
        if (strstr(body, "\"model\":\"arrcontent\"") != NULL) {
            /* antwort mit content als array -> text-parts konkatenieren */
            send_json(fd, "200 OK",
                      "{\"id\":\"c3\",\"model\":\"arrcontent\",\"choices\":"
                      "[{\"index\":0,\"message\":{\"role\":\"assistant\","
                      "\"content\":[{\"type\":\"text\",\"text\":\"teil\"},"
                      "{\"type\":\"text\",\"text\":\"eins\"}]},"
                      "\"finish_reason\":\"stop\"}]}}");
            continue;
        }
        if (strstr(body, "\"model\":\"bigbody\"") != NULL) {
            /* riesige antwort: client mit body-limit muss abbrechen */
            char head[128];
            int hl = snprintf(head, sizeof head,
                              "HTTP/1.1 200 OK\r\n"
                              "Content-Type: application/json\r\n"
                              "Content-Length: 65536\r\n\r\n");
            if (hl > 0) {
                write(fd, head, (size_t)hl);
            }
            static char xs[4096];
            memset(xs, 'x', sizeof xs);
            for (int i = 0; i < 16; i++) {
                if (write(fd, xs, sizeof xs) != (ssize_t)sizeof xs) {
                    break;
                }
            }
            return;
        }
        if (strstr(body, "\"model\":\"stalledstream\"") != NULL) {
            /* ein event, dann stille -> client muss idle-abbruch machen */
            const char *resp =
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: text/event-stream\r\n"
                "Connection: close\r\n\r\n"
                "data: {\"id\":\"c\",\"model\":\"t\",\"choices\":[{"
                "\"index\":0,\"delta\":{\"content\":\"hal\"}}]}\n\n";
            write(fd, resp, strlen(resp));
            char tmp[64];
            for (;;) {
                if (read(fd, tmp, sizeof tmp) <= 0) {
                    return;
                }
            }
        }
        if (strstr(body, "\"model\":\"truncstream\"") != NULL) {
            /* stream ohne [DONE], verbindung wird einfach geschlossen */
            const char *resp =
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: text/event-stream\r\n"
                "Connection: close\r\n\r\n"
                "data: {\"id\":\"c\",\"model\":\"t\",\"choices\":[{"
                "\"index\":0,\"delta\":{\"content\":\"hal\"}}]}\n\n";
            write(fd, resp, strlen(resp));
            return; /* verbindung zu, ohne [DONE] */
        }
        if (strstr(body, "\"stream\":true") != NULL) {
            /* sse, inklusive crlf-zeilenenden und einem kaputten event, */
            /* das ignoriert werden muss */
            const char *resp =
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: text/event-stream\r\n"
                "Connection: close\r\n\r\n"
                "data: {\"id\":\"c1\",\"model\":\"t\",\"choices\":[{"
                "\"index\":0,\"delta\":{\"role\":\"assistant\","
                "\"content\":\"hal\"}}]}\n\n"
                "data: {invalid json!}\n\n"
                "data: {\"id\":\"c1\",\"model\":\"t\",\"choices\":[{"
                "\"index\":0,\"delta\":{\"content\":\"lo\"}}]}\r\n\r\n"
                "data: {\"id\":\"c1\",\"model\":\"t\",\"choices\":[{"
                "\"index\":0,\"delta\":{},\"finish_reason\":\"stop\"}]}\n\n"
                "data: [DONE]\n\n";
            write(fd, resp, strlen(resp));
            return;
        }
        if (strstr(body, "\"model\":\"tooltest\"") != NULL) {
            send_json(fd, "200 OK",
                      "{\"id\": \"c2\", \"model\": \"tooltest\", \"choices\": "
                      "[{\"index\": 0, \"message\": {\"role\": \"assistant\", "
                      "\"content\": null, \"tool_calls\": [{\"id\": "
                      "\"call_1\", \"type\": \"function\", \"function\": "
                      "{\"name\": \"get_weather\", \"arguments\": "
                      "\"{\\\"city\\\":\\\"Berlin\\\"}\"}}]}, "
                      "\"finish_reason\": \"tool_calls\"}]}");
            continue;
        }

        /* default: normale completion */
        send_json(fd, "200 OK", default_json());
        continue;
    }
}

static pid_t start_server(int *port)
{
    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    if (lfd < 0) {
        return -1;
    }
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_LOOPBACK),
        .sin_port = 0,
    };
    if (bind(lfd, (struct sockaddr *)&addr, sizeof addr) != 0 ||
        listen(lfd, 8) != 0) {
        close(lfd);
        return -1;
    }
    socklen_t slen = sizeof addr;
    getsockname(lfd, (struct sockaddr *)&addr, &slen);
    *port = ntohs(addr.sin_port);

    pid_t pid = fork();
    if (pid < 0) {
        close(lfd);
        return -1;
    }
    if (pid == 0) {
        signal(SIGPIPE, SIG_IGN);
        for (;;) {
            int cfd = accept(lfd, NULL, NULL);
            if (cfd < 0) {
                continue;
            }
            handle_conn(cfd);
            close(cfd);
        }
    }
    close(lfd);
    return pid;
}

/* ------------------------------------------------------------------ */
/* streaming-akkumulator                                               */
/* ------------------------------------------------------------------ */

typedef struct {
    char text[512];
    char reasoning[512]; /* reasoning_content-deltas, konkateniert */
    int chunks;
    int finish_seen;
    int usage_seen;
    int usage_total;
    int usage_cached;
} StreamAcc;

typedef struct {
    StreamAcc acc;
    int error_seen;
    long error_status;
    char error_msg[256];
} StreamState;

static void on_error(long http_status, const char *message, void *user_data)
{
    StreamState *state = user_data;
    state->error_seen = 1;
    state->error_status = http_status;
    snprintf(state->error_msg, sizeof state->error_msg, "%s", message);
}

static int on_chunk(const OaiChatCompletionChunk *chunk, void *user_data)
{
    StreamAcc *acc = user_data;
    acc->chunks++;
    for (size_t i = 0; i < chunk->choices_len; i++) {
        const OaiChunkChoice *c = &chunk->choices[i];
        if (c->content_delta != NULL) {
            size_t len = strlen(acc->text);
            snprintf(acc->text + len, sizeof acc->text - len, "%s",
                     c->content_delta);
        }
        if (c->reasoning_delta != NULL) {
            size_t len = strlen(acc->reasoning);
            snprintf(acc->reasoning + len, sizeof acc->reasoning - len, "%s",
                     c->reasoning_delta);
        }
        if (c->finish_reason != NULL) {
            acc->finish_seen = 1;
        }
    }
    if (chunk->has_usage) {
        acc->usage_seen = 1;
        acc->usage_total = chunk->usage.total_tokens;
        acc->usage_cached = chunk->usage.cached_tokens;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* tests                                                               */
/* ------------------------------------------------------------------ */

int main(void)
{
    int port = 0;
    pid_t server = start_server(&port);
    if (server < 0) {
        fprintf(stderr, "mock-server konnte nicht starten\n");
        return 1;
    }

    char base_url[64];
    snprintf(base_url, sizeof base_url, "http://127.0.0.1:%d/v1", port);

    OaiClient client;
    OaiClientOptions opts = {
        .api_key = "test-key",
        .base_url = base_url,
        .max_retries = 0,
    };
    CHECK(oai_client_init(&client, &opts) == 0);

    /* 1) einfache anfrage: content, finish_reason, usage */
    {
        OaiMessage messages[] = {{.role = OAI_ROLE_USER, .content = "hi"}};
        OaiChatCompletionParams params = {
            .model = "test",
            .messages = messages,
            .messages_len = 1,
        };
        OaiCompletionResult result;
        CHECK(oai_chat_completions_create(&client, &params, &result) == 0);
        CHECK(result.ok);
        CHECK(result.http_status == 200);
        CHECK(result.completion.choices_len == 1);
        CHECK(strcmp(result.completion.choices[0].message.content, "antwort") ==
              0);
        CHECK(strcmp(result.completion.choices[0].finish_reason, "stop") == 0);
        CHECK(result.completion.has_usage);
        CHECK(result.completion.usage.total_tokens == 7);
        oai_completion_result_free(&result);
        /* free ist idempotent */
        oai_completion_result_free(&result);
    }

    /* 2) keep-alive: zweite anfrage ueber denselben client */
    {
        OaiMessage messages[] = {{.role = OAI_ROLE_USER, .content = "hi"}};
        OaiChatCompletionParams params = {
            .model = "test",
            .messages = messages,
            .messages_len = 1,
        };
        OaiCompletionResult result;
        CHECK(oai_chat_completions_create(&client, &params, &result) == 0);
        CHECK(result.ok);
        oai_completion_result_free(&result);
    }

    /* 3) tool-calls parsen + tool-loop-roundtrip */
    {
        OaiMessage messages[] = {{.role = OAI_ROLE_USER, .content = "wetter"}};
        OaiChatCompletionParams params = {
            .model = "tooltest",
            .messages = messages,
            .messages_len = 1,
        };
        OaiCompletionResult result;
        CHECK(oai_chat_completions_create(&client, &params, &result) == 0);
        CHECK(result.ok);
        OaiResponseMessage *msg = &result.completion.choices[0].message;
        CHECK(msg->tool_calls_len == 1);
        CHECK(strcmp(msg->tool_calls[0].id, "call_1") == 0);
        CHECK(strcmp(msg->tool_calls[0].name, "get_weather") == 0);
        CHECK(strcmp(msg->tool_calls[0].arguments, "{\"city\":\"Berlin\"}") ==
              0);
        CHECK(strcmp(result.completion.choices[0].finish_reason,
                     "tool_calls") == 0);
        oai_completion_result_free(&result);
    }

    /* 4) streaming: inkl. kaputtem event und crlf-trenner */
    {
        OaiMessage messages[] = {{.role = OAI_ROLE_USER, .content = "hi"}};
        OaiChatCompletionParams params = {
            .model = "test",
            .messages = messages,
            .messages_len = 1,
        };
        StreamState state = {0};
        OaiStreamCallbacks cbs = {
            .on_chunk = on_chunk, .on_error = on_error, .user_data = &state};
        CHECK(oai_chat_completions_create_stream(&client, &params, &cbs) == 0);
        CHECK(strcmp(state.acc.text, "hallo") == 0);
        CHECK(state.acc.chunks == 3);
        CHECK(state.acc.finish_seen);
    }

    /* 5) http-fehler: meldung aus dem body extrahiert */
    {
        OaiMessage messages[] = {{.role = OAI_ROLE_USER, .content = "hi"}};
        OaiChatCompletionParams params = {
            .model = "error",
            .messages = messages,
            .messages_len = 1,
        };
        OaiCompletionResult result;
        CHECK(oai_chat_completions_create(&client, &params, &result) == 0);
        CHECK(!result.ok);
        CHECK(result.http_status == 401);
        CHECK(strstr(result.error, "invalid api key") != NULL);
        oai_completion_result_free(&result);
    }

    /* 6) kaputte json-antwort */
    {
        OaiMessage messages[] = {{.role = OAI_ROLE_USER, .content = "hi"}};
        OaiChatCompletionParams params = {
            .model = "malformed",
            .messages = messages,
            .messages_len = 1,
        };
        OaiCompletionResult result;
        CHECK(oai_chat_completions_create(&client, &params, &result) == 0);
        CHECK(!result.ok);
        oai_completion_result_free(&result);
    }

    /* 7) stream ohne [DONE]: fehler statt halbe antwort */
    {
        OaiMessage messages[] = {{.role = OAI_ROLE_USER, .content = "hi"}};
        OaiChatCompletionParams params = {
            .model = "truncstream",
            .messages = messages,
            .messages_len = 1,
        };
        StreamState state = {0};
        OaiStreamCallbacks cbs = {
            .on_chunk = on_chunk, .on_error = on_error, .user_data = &state};
        CHECK(oai_chat_completions_create_stream(&client, &params, &cbs) == 0);
        CHECK(state.error_seen);
        CHECK(strstr(state.error_msg, "[DONE]") != NULL);
    }

    /* 8) ungueltige parameter: NULL model / NULL messages */
    {
        OaiMessage messages[] = {{.role = OAI_ROLE_USER, .content = "hi"}};
        OaiChatCompletionParams params = {
            .model = NULL,
            .messages = messages,
            .messages_len = 1,
        };
        OaiCompletionResult result;
        CHECK(oai_chat_completions_create(&client, &params, &result) == -1);
        OaiChatCompletionParams params2 = {.model = "test"};
        CHECK(oai_chat_completions_create(&client, &params2, &result) == -1);
    }

    /* 9) retry: 500 beim ersten versuch, erfolg beim zweiten */
    {
        OaiClient retry_client;
        OaiClientOptions retry_opts = {
            .api_key = "test-key",
            .base_url = base_url,
            .max_retries = 2,
        };
        CHECK(oai_client_init(&retry_client, &retry_opts) == 0);
        OaiMessage messages[] = {{.role = OAI_ROLE_USER, .content = "hi"}};
        OaiChatCompletionParams params = {
            .model = "flaky",
            .messages = messages,
            .messages_len = 1,
        };
        OaiCompletionResult result;
        CHECK(oai_chat_completions_create(&retry_client, &params, &result) ==
              0);
        CHECK(result.ok);
        oai_completion_result_free(&result);
        oai_client_free(&retry_client);
    }

    /* 9b) 409 wird wie im npm-package wiederholt */
    {
        OaiClient conflict_client;
        OaiClientOptions conflict_opts = {
            .api_key = "test-key",
            .base_url = base_url,
            .max_retries = 2,
        };
        CHECK(oai_client_init(&conflict_client, &conflict_opts) == 0);
        OaiMessage messages[] = {{.role = OAI_ROLE_USER, .content = "hi"}};
        OaiChatCompletionParams params = {
            .model = "conflict",
            .messages = messages,
            .messages_len = 1,
        };
        OaiCompletionResult result;
        CHECK(oai_chat_completions_create(&conflict_client, &params, &result) ==
              0);
        CHECK(result.ok);
        oai_completion_result_free(&result);
        oai_client_free(&conflict_client);
    }

    /* 9c) 429 mit retry-after: 0 wird wiederholt (header wird geparst) */
    {
        OaiClient rate_client;
        OaiClientOptions rate_opts = {
            .api_key = "test-key",
            .base_url = base_url,
            .max_retries = 2,
        };
        CHECK(oai_client_init(&rate_client, &rate_opts) == 0);
        OaiMessage messages[] = {{.role = OAI_ROLE_USER, .content = "hi"}};
        OaiChatCompletionParams params = {
            .model = "rate",
            .messages = messages,
            .messages_len = 1,
        };
        OaiCompletionResult result;
        CHECK(oai_chat_completions_create(&rate_client, &params, &result) == 0);
        CHECK(result.ok);
        oai_completion_result_free(&result);
        oai_client_free(&rate_client);
    }

    /* 9d) deterministischer fehler (invalides tool-schema) wird NICHT */
    /* wiederholt: keine retries, kein backoff, keine server-anfrage */
    {
        OaiClient schema_client;
        OaiClientOptions schema_opts = {
            .api_key = "test-key",
            .base_url = base_url,
            .max_retries = 5,
        };
        CHECK(oai_client_init(&schema_client, &schema_opts) == 0);
        OaiTool tools[] = {
            {.function = {.name = "f", .parameters_json = "{kein json"}}};
        OaiMessage messages[] = {{.role = OAI_ROLE_USER, .content = "hi"}};
        OaiChatCompletionParams params = {
            .model = "test",
            .messages = messages,
            .messages_len = 1,
            .tools = tools,
            .tools_len = 1,
        };
        struct timespec t0;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        OaiCompletionResult result;
        CHECK(oai_chat_completions_create(&schema_client, &params, &result) ==
              0);
        struct timespec t1;
        clock_gettime(CLOCK_MONOTONIC, &t1);
        double elapsed = (double)(t1.tv_sec - t0.tv_sec) +
                         ((double)(t1.tv_nsec - t0.tv_nsec) / 1e9);
        CHECK(!result.ok);
        CHECK(strstr(result.error, "tool-schema") != NULL);
        /* mit retries+backoff waeren es > 0.5+1+2+4+8s; ohne < 1s */
        CHECK(elapsed < 1.0);
        printf("  no-retry: %.3fs (erwartet < 1s, mit retries > 15s)\n",
               elapsed);
        oai_completion_result_free(&result);
        oai_client_free(&schema_client);
    }

    /* 9e) fragmentierter sse-stream (echte tcp-pakete) + [DONE] mit */
    /* trailing-space */
    {
        OaiMessage messages[] = {{.role = OAI_ROLE_USER, .content = "hi"}};
        OaiChatCompletionParams params = {
            .model = "slowstream",
            .messages = messages,
            .messages_len = 1,
        };
        StreamState state = {0};
        OaiStreamCallbacks cbs = {
            .on_chunk = on_chunk, .on_error = on_error, .user_data = &state};
        CHECK(oai_chat_completions_create_stream(&client, &params, &cbs) == 0);
        CHECK(strcmp(state.acc.text, "hallo") == 0);
        CHECK(state.acc.chunks == 3);
        CHECK(state.acc.finish_seen);
        CHECK(!state.error_seen);
    }

    /* 10) verbindungsaufbau zu geschlossenem port */
    {
        int dead = socket(AF_INET, SOCK_STREAM, 0);
        struct sockaddr_in daddr = {
            .sin_family = AF_INET,
            .sin_addr.s_addr = htonl(INADDR_LOOPBACK),
            .sin_port = 0,
        };
        CHECK(bind(dead, (struct sockaddr *)&daddr, sizeof daddr) == 0);
        socklen_t dlen = sizeof daddr;
        CHECK(getsockname(dead, (struct sockaddr *)&daddr, &dlen) == 0);
        int dead_port = ntohs(daddr.sin_port);
        close(dead);

        char dead_url[64];
        snprintf(dead_url, sizeof dead_url, "http://127.0.0.1:%d/v1",
                 dead_port);
        OaiClient dead_client;
        OaiClientOptions dead_opts = {
            .api_key = "test-key",
            .base_url = dead_url,
            .max_retries = 0,
        };
        CHECK(oai_client_init(&dead_client, &dead_opts) == 0);
        OaiMessage messages[] = {{.role = OAI_ROLE_USER, .content = "hi"}};
        OaiChatCompletionParams params = {
            .model = "test",
            .messages = messages,
            .messages_len = 1,
        };
        OaiCompletionResult result;
        CHECK(oai_chat_completions_create(&dead_client, &params, &result) == 0);
        CHECK(!result.ok);
        CHECK(result.http_status == 0); /* verbindungsfehler */
        oai_completion_result_free(&result);
        oai_client_free(&dead_client);
    }

    /* 11) neue request-felder: content-parts (vision), tool_choice,     */
    /* parallel_tool_calls, seed, response_format                          */
    {
        OaiContentPart parts[] = {
            {.type = OAI_CONTENT_TEXT, .text = "hi"},
            {.type = OAI_CONTENT_IMAGE_URL,
             .image_url = "data:image/png;base64,QUJD"},
        };
        OaiMessage messages[] = {{.role = OAI_ROLE_USER,
                                  .content_parts = parts,
                                  .content_parts_len = 2}};
        OaiChatCompletionParams params = {
            .model = "full",
            .messages = messages,
            .messages_len = 1,
            .has_tool_choice = true,
            .tool_choice = OAI_TOOL_CHOICE_AUTO,
            .has_parallel_tool_calls = true,
            .parallel_tool_calls = false,
            .has_seed = true,
            .seed = 42,
            .has_response_format = true,
            .response_format_type = "json_object",
        };
        OaiCompletionResult result;
        CHECK(oai_chat_completions_create(&client, &params, &result) == 0);
        if (!result.ok) {
            fprintf(stderr, "  full-params-fehler: %s\n", result.error);
        }
        CHECK(result.ok);
        oai_completion_result_free(&result);
    }

    /* 12) tool_choice named + stream_options.include_usage (usage im    */
    /* letzten chunk)                                                       */
    {
        OaiToolFunction weather_fn = {
            .name = "get_weather", .parameters_json = "{\"type\":\"object\"}"};
        OaiTool tools[] = {{.function = weather_fn}};
        OaiMessage messages[] = {{.role = OAI_ROLE_USER, .content = "wetter"}};
        OaiChatCompletionParams params = {
            .model = "fullstream",
            .messages = messages,
            .messages_len = 1,
            .tools = tools,
            .tools_len = 1,
            .has_tool_choice = true,
            .tool_choice = OAI_TOOL_CHOICE_FUNCTION,
            .tool_choice_function = "get_weather",
            .include_usage = true,
        };
        StreamState state = {0};
        OaiStreamCallbacks cbs = {
            .on_chunk = on_chunk, .on_error = on_error, .user_data = &state};
        CHECK(oai_chat_completions_create_stream(&client, &params, &cbs) == 0);
        if (state.error_seen) {
            fprintf(stderr, "  fullstream-fehler: %s\n", state.error_msg);
        }
        CHECK(!state.error_seen);
        CHECK(strcmp(state.acc.text, "hal") == 0);
        CHECK(state.acc.usage_seen);
        CHECK(state.acc.usage_total == 12);
        CHECK(state.acc.finish_seen);
    }

    /* 13) antwort mit content als array: text-parts konkatenieren */
    {
        OaiMessage messages[] = {{.role = OAI_ROLE_USER, .content = "hi"}};
        OaiChatCompletionParams params = {
            .model = "arrcontent",
            .messages = messages,
            .messages_len = 1,
        };
        OaiCompletionResult result;
        CHECK(oai_chat_completions_create(&client, &params, &result) == 0);
        CHECK(result.ok);
        CHECK(strcmp(result.completion.choices[0].message.content,
                     "teileins") == 0);
        oai_completion_result_free(&result);
    }

    /* 14) reasoning-modell, nicht-streaming: thinking in
     * reasoning_content, cache-anteil unter prompt_tokens_details */
    {
        OaiMessage messages[] = {{.role = OAI_ROLE_USER, .content = "hi"}};
        OaiChatCompletionParams params = {
            .model = "reasoning",
            .messages = messages,
            .messages_len = 1,
        };
        OaiCompletionResult result;
        CHECK(oai_chat_completions_create(&client, &params, &result) == 0);
        CHECK(result.ok);
        OaiResponseMessage *msg = &result.completion.choices[0].message;
        CHECK(strcmp(msg->content, "antwort") == 0);
        CHECK(msg->reasoning != NULL &&
              strcmp(msg->reasoning, "ich denke") == 0);
        CHECK(result.completion.usage.cached_tokens == 4);

        /* roundtrip: dieselbe nachricht mit thinking zurueckspielen
         * – reasoning_content MUSS mit raus (agent-loop) */
        OaiMessage back[] = {
            {.role = OAI_ROLE_USER, .content = "hi"},
            {.role = OAI_ROLE_ASSISTANT,
             .content = "antwort",
             .reasoning = msg->reasoning},
        };
        OaiChatCompletionParams params2 = {
            .model = "reasonround",
            .messages = back,
            .messages_len = 2,
        };
        OaiCompletionResult r2;
        CHECK(oai_chat_completions_create(&client, &params2, &r2) == 0);
        CHECK(r2.ok);
        oai_completion_result_free(&r2);
        oai_completion_result_free(&result);
    }

    /* 15) reasoning-stream: thinking-deltas + reasoning_effort im
     * request + cache-anteil top-level (ollama-form) */
    {
        OaiMessage messages[] = {{.role = OAI_ROLE_USER, .content = "hi"}};
        OaiChatCompletionParams params = {
            .model = "reasonstream",
            .messages = messages,
            .messages_len = 1,
            .include_usage = true,
            .has_reasoning_effort = true,
            .reasoning_effort = "high",
        };
        StreamState state = {0};
        OaiStreamCallbacks cbs = {
            .on_chunk = on_chunk, .on_error = on_error, .user_data = &state};
        CHECK(oai_chat_completions_create_stream(&client, &params, &cbs) == 0);
        if (state.error_seen) {
            fprintf(stderr, "  reasonstream-fehler: %s\n", state.error_msg);
        }
        CHECK(!state.error_seen);
        CHECK(strcmp(state.acc.text, "ant") == 0);
        CHECK(strcmp(state.acc.reasoning, "denke") == 0);
        CHECK(state.acc.usage_seen);
        CHECK(state.acc.usage_cached == 6);
        CHECK(state.acc.finish_seen);
    }

    /* 14) stream-idle-timeout: stalleder stream wird nach 1s abgebrochen */
    /* der mock-server ist single-threaded: die keep-alive-verbindung des   */
    /* haupt-clients wuerde ihn blockieren -> client vorher freigeben      */
    oai_client_free(&client);
    {
        OaiClient stall_client;
        OaiClientOptions stall_opts = {
            .api_key = "test-key",
            .base_url = base_url,
            .max_retries = 0,
            .stream_idle_timeout_ms = 1000,
        };
        CHECK(oai_client_init(&stall_client, &stall_opts) == 0);
        OaiMessage messages[] = {{.role = OAI_ROLE_USER, .content = "hi"}};
        OaiChatCompletionParams params = {
            .model = "stalledstream",
            .messages = messages,
            .messages_len = 1,
        };
        StreamState state = {0};
        OaiStreamCallbacks cbs = {
            .on_chunk = on_chunk, .on_error = on_error, .user_data = &state};
        struct timespec t0;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        CHECK(oai_chat_completions_create_stream(&stall_client, &params,
                                                 &cbs) == 0);
        struct timespec t1;
        clock_gettime(CLOCK_MONOTONIC, &t1);
        double elapsed = (double)(t1.tv_sec - t0.tv_sec) +
                         ((double)(t1.tv_nsec - t0.tv_nsec) / 1e9);
        CHECK(state.error_seen);
        CHECK(strstr(state.error_msg, "idle") != NULL);
        CHECK(strcmp(state.acc.text, "hal") == 0); /* daten kamen an */
        CHECK(elapsed < 6.0); /* idle-abbruch, kein ewiges haengen */
        CHECK(elapsed > 0.5);
        printf("  idle-timeout: %.3fs\n", elapsed);
        oai_client_free(&stall_client);
    }

    /* 15) body-limit: riesige antwort wird abgebrochen statt OOM */
    {
        OaiClient big_client;
        OaiClientOptions big_opts = {
            .api_key = "test-key",
            .base_url = base_url,
            .max_retries = 2,
            .max_body_bytes = 1024,
        };
        CHECK(oai_client_init(&big_client, &big_opts) == 0);
        OaiMessage messages[] = {{.role = OAI_ROLE_USER, .content = "hi"}};
        OaiChatCompletionParams params = {
            .model = "bigbody",
            .messages = messages,
            .messages_len = 1,
        };
        struct timespec t0;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        OaiCompletionResult result;
        CHECK(oai_chat_completions_create(&big_client, &params, &result) == 0);
        struct timespec t1;
        clock_gettime(CLOCK_MONOTONIC, &t1);
        double elapsed = (double)(t1.tv_sec - t0.tv_sec) +
                         ((double)(t1.tv_nsec - t0.tv_nsec) / 1e9);
        CHECK(!result.ok);
        CHECK(strstr(result.error, "zu gross") != NULL);
        CHECK(elapsed < 2.0); /* ohne no_retry waeren es > 15s backoff */
        printf("  body-limit: %.3fs\n", elapsed);
        oai_completion_result_free(&result);
        oai_client_free(&big_client);
    }

    /* free auf zero-initialisiertem client ist safe */
    oai_client_free(&client);

    kill(server, SIGKILL);
    waitpid(server, NULL, 0);

    return test_report();
}