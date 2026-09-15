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
    int chunks;
    int finish_seen;
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
        if (c->finish_reason != NULL) {
            acc->finish_seen = 1;
        }
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

    oai_client_free(&client);
    /* free auf zero-initialisiertem client ist safe */
    oai_client_free(&client);

    kill(server, SIGKILL);
    waitpid(server, NULL, 0);

    return test_report();
}