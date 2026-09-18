#define _POSIX_C_SOURCE 200809L // NOLINT(bugprone-reserved-identifier)

#include <arpa/inet.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* marker, an dem sich ablesen laesst, ob das mock-tool wirklich
 * gelaufen ist (die rueckfrage-tests haengen daran) */
#define CONFIRM_MARK "/tmp/max-agent-confirm.mark"

#include "chat.h"
#include "keys.h"
#include "send.h"
#include "state.h"
#include "test.h"
#include "utils.h"

/* test-config im stil von tests/test_config.c: ein provider mit
 * zwei modellen (zweiter ohne api-key, um auch den abzufragen) */
static void build_cfg(Config *cfg)
{
    memset(cfg, 0, sizeof *cfg);
    cfg->providers = calloc(2, sizeof(Provider));
    if (!cfg->providers) {
        die("out of memory");
    }
    cfg->providers_len = 2;

    Provider *p0 = &cfg->providers[0];
    p0->api_key = dup_str("sk-test-123");
    p0->base_url = dup_str("https://api.example.com/v1");
    p0->models = calloc(2, sizeof(Model));
    p0->models_len = 2;
    p0->models[0].id = dup_str("gpt-test");
    p0->models[1].id = dup_str("mini-test");

    Provider *p1 = &cfg->providers[1];
    p1->api_key = NULL; /* ohne key: send_message muss das melden */
    p1->base_url = dup_str("http://localhost:11434/v1");
    p1->models = calloc(1, sizeof(Model));
    p1->models_len = 1;
    p1->models[0].id = dup_str("local-model");
}

/* ------------------------------------------------------------------ */
/* send_stream: gegen einen minimalen sse-mock im child-prozess      */
/* (vorbild: tests/test_openai_completions.c). der server nimmt eine  */
/* verbindung an, antwortet mit drei chunks + usage + [DONE].         */
/* ------------------------------------------------------------------ */

/* einen request komplett lesen (header + body per content-length),
 * damit der server erst antwortet, wenn alles angekommen ist – die
 * requests sind seit den tool-definitionen gross genug, dass ein
 * fruehes close die verbindung zerreisst */
static int read_request(int cfd, char *buf, size_t cap)
{
    size_t got = 0;
    while (got < cap - 1 && strstr(buf, "\r\n\r\n") == NULL) {
        ssize_t n = read(cfd, buf + got, cap - 1 - got);
        if (n <= 0) {
            return -1;
        }
        got += (size_t)n;
        buf[got] = '\0'; // NOLINT(clang-analyzer-security.ArrayBound)
    }
    const char *cl = strstr(buf, "Content-Length:");
    if (cl == NULL) {
        return 0; /* ohne body: gut genug */
    }
    size_t body = (size_t)strtol(cl + 15, NULL, 10);
    if (body > cap - 1) {
        body = cap - 1; /* groessere bodies kuerzen, reicht zum routen */
    }
    size_t header_end = (size_t)(strstr(buf, "\r\n\r\n") + 4 - buf);
    while (got < header_end + body) {
        ssize_t n = read(cfd, buf + got, cap - 1 - got);
        if (n <= 0) {
            break;
        }
        got += (size_t)n;
        buf[got] = '\0'; // NOLINT(clang-analyzer-security.ArrayBound)
    }
    return 0;
}

static pid_t start_sse_server(int *port)
{
    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    if (lfd < 0) {
        return -1;
    }
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (bind(lfd, (struct sockaddr *)&addr, sizeof addr) != 0 ||
        listen(lfd, 4) != 0 ||
        getsockname(lfd, (struct sockaddr *)&addr, &(socklen_t){sizeof addr}) !=
            0) {
        close(lfd);
        return -1;
    }
    *port = ntohs(addr.sin_port);

    pid_t pid = fork();
    if (pid != 0) {
        close(lfd);
        return pid; /* parent: weiter im test */
    }

    /* child: eine verbindung, request komplett lesen, sse
     * zurueckschreiben, fertig */
    int cfd = accept(lfd, NULL, NULL);
    if (cfd < 0) {
        _exit(1);
    }
    char buf[16384];
    buf[0] = '\0';
    (void)read_request(cfd, buf, sizeof buf);
    static const char resp[] =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/event-stream\r\n"
        "Connection: close\r\n"
        "\r\n"
        "data: {\"choices\":[{\"delta\":{\"role\":\"assistant\"}}]}\n\n"
        "data: {\"choices\":[{\"delta\":{\"content\":\"hal\"}}]}\n\n"
        "data: {\"choices\":[{\"delta\":{\"content\":\"lo\"}}]}\n\n"
        "data: {\"usage\":{\"prompt_tokens\":5,\"completion_tokens\":2,"
        "\"total_tokens\":7},\"choices\":[]}\n\n"
        "data: [DONE]\n\n";
    (void)write(cfd, resp, sizeof resp - 1);
    close(cfd);
    _exit(0);
}

/* server fuer die busy-queue: ZWEI antworten auf einer verbindung
 * (keep-alive), der client (send_stream) nutzt seinen verbindungs-
 * cache. so kann ein test den ersten turn und das automatische
 * nachsenden der queue gegen denselben port fahren */
static pid_t start_two_sse_server(int *port)
{
    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    if (lfd < 0) {
        return -1;
    }
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (bind(lfd, (struct sockaddr *)&addr, sizeof addr) != 0 ||
        listen(lfd, 4) != 0 ||
        getsockname(lfd, (struct sockaddr *)&addr, &(socklen_t){sizeof addr}) !=
            0) {
        close(lfd);
        return -1;
    }
    *port = ntohs(addr.sin_port);

    pid_t pid = fork();
    if (pid != 0) {
        close(lfd);
        return pid;
    }

    /* ZWEI getrennte verbindungen: send_stream baut pro turn einen
     * eigenen client (der verbindungs-cache gilt nur fuer die
     * runden EINES turns). jede verbindung bekommt eine antwort */
    for (int conn = 0; conn < 2; conn++) {
        int cfd = accept(lfd, NULL, NULL);
        if (cfd < 0) {
            _exit(1);
        }
        char buf[16384];
        static const char resp[] = "HTTP/1.1 200 OK\r\n"
                                   "Content-Type: text/event-stream\r\n"
                                   "Connection: close\r\n"
                                   "Content-Length: %d\r\n"
                                   "\r\n"
                                   "%s";
        static const char body[] =
            "data: {\"choices\":[{\"delta\":{\"content\":\"ok\"}}]}\n\n"
            "data: [DONE]\n\n";
        int blen = (int)(sizeof body - 1);
        char head[256];
        buf[0] = '\0';
        (void)read_request(cfd, buf, sizeof buf);
        int hlen = snprintf(head, sizeof head, resp, blen, body);
        (void)write(cfd, head, (size_t)hlen);
        close(cfd);
    }
    _exit(0);
}

/* wie start_sse_server, aber mit frei waehlbarer antwort: fuer
 * tests, die eine einzelne, gezielt kaputte sse-antwort brauchen */
static pid_t start_once_server(int *port, const char *resp)
{
    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    if (lfd < 0) {
        return -1;
    }
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (bind(lfd, (struct sockaddr *)&addr, sizeof addr) != 0 ||
        listen(lfd, 4) != 0 ||
        getsockname(lfd, (struct sockaddr *)&addr, &(socklen_t){sizeof addr}) !=
            0) {
        close(lfd);
        return -1;
    }
    *port = ntohs(addr.sin_port);

    pid_t pid = fork();
    if (pid != 0) {
        close(lfd);
        return pid;
    }

    int cfd = accept(lfd, NULL, NULL);
    if (cfd < 0) {
        _exit(1);
    }
    char buf[16384];
    buf[0] = '\0';
    (void)read_request(cfd, buf, sizeof buf);
    (void)write(cfd, resp, strlen(resp));
    close(cfd);
    _exit(0);
}

/* server, der im request-body nach einem marker sucht und das
 * ergebnis als antwort zurueckgibt ("JA"/"NEIN") – damit laesst
 * sich pruefen, WAS tatsaechlich rausgegangen ist. der usage-chunk
 * am ende eicht nebenbei die token-schaetzung. */
static pid_t start_probe_server(int *port, const char *marker)
{
    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    if (lfd < 0) {
        return -1;
    }
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (bind(lfd, (struct sockaddr *)&addr, sizeof addr) != 0 ||
        listen(lfd, 4) != 0 ||
        getsockname(lfd, (struct sockaddr *)&addr, &(socklen_t){sizeof addr}) !=
            0) {
        close(lfd);
        return -1;
    }
    *port = ntohs(addr.sin_port);

    pid_t pid = fork();
    if (pid != 0) {
        close(lfd);
        return pid;
    }

    int cfd = accept(lfd, NULL, NULL);
    if (cfd < 0) {
        _exit(1);
    }
    /* der body traegt den ganzen verlauf: gross genug lesen */
    char *req = malloc(1 << 20);
    if (req == NULL) {
        _exit(1);
    }
    req[0] = '\0';
    (void)read_request(cfd, req, 1 << 20);
    const char *found = (strstr(req, marker) != NULL) ? "JA" : "NEIN";
    free(req);

    char resp[512];
    (void)snprintf(resp, sizeof resp,
                   "HTTP/1.1 200 OK\r\n"
                   "Content-Type: text/event-stream\r\n"
                   "Connection: close\r\n"
                   "\r\n"
                   "data: {\"choices\":[{\"delta\":{\"content\":\"%s\"}}]}\n\n"
                   "data: {\"usage\":{\"prompt_tokens\":1234,"
                   "\"completion_tokens\":2,\"total_tokens\":1236},"
                   "\"choices\":[]}\n\n"
                   "data: [DONE]\n\n",
                   found);
    (void)write(cfd, resp, strlen(resp));
    close(cfd);
    _exit(0);
}

/* server, der ZWEI verbindungen bedient: die erste bekommt eine
 * feste summary-antwort (der compaction-request), die zweite ist
 * ein marker-probe wie oben (der haupt-request). damit laesst sich
 * compaction + normaler round-trip in einem test fahren. */
static pid_t start_compact_probe_server(int *port, const char *marker,
                                        const char *summary)
{
    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    if (lfd < 0) {
        return -1;
    }
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (bind(lfd, (struct sockaddr *)&addr, sizeof addr) != 0 ||
        listen(lfd, 4) != 0 ||
        getsockname(lfd, (struct sockaddr *)&addr, &(socklen_t){sizeof addr}) !=
            0) {
        close(lfd);
        return -1;
    }
    *port = ntohs(addr.sin_port);

    pid_t pid = fork();
    if (pid != 0) {
        close(lfd);
        return pid;
    }

    char *req = malloc(1 << 20);
    if (req == NULL) {
        _exit(1);
    }
    for (int round = 0; round < 2; round++) {
        int cfd = accept(lfd, NULL, NULL);
        if (cfd < 0) {
            _exit(1);
        }
        req[0] = '\0';
        (void)read_request(cfd, req, 1 << 20);
        const char *content;
        if (round == 0) {
            /* compaction-request: antwortet mit der summary */
            content = summary;
        } else {
            content = (strstr(req, marker) != NULL) ? "JA" : "NEIN";
        }
        char resp[512];
        (void)snprintf(
            resp, sizeof resp,
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: text/event-stream\r\n"
            "Connection: close\r\n"
            "\r\n"
            "data: {\"choices\":[{\"delta\":{\"content\":\"%s\"}}]}\n\n"
            "data: {\"usage\":{\"prompt_tokens\":1234,"
            "\"completion_tokens\":2,\"total_tokens\":1236},"
            "\"choices\":[]}\n\n"
            "data: [DONE]\n\n",
            content);
        (void)write(cfd, resp, strlen(resp));
        close(cfd);
    }
    free(req);
    _exit(0);
}

static void sleep_ms(long ms)
{
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

/* server, der den stream BEWUSST in die laenge zieht: erst ein
 * fragment, dann eine pause, dann der rest. nur so kommt der
 * idle-watchdog des clients (und damit der abbruch-callback)
 * ueberhaupt zum zug. */
static pid_t start_slow_server(int *port)
{
    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    if (lfd < 0) {
        return -1;
    }
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (bind(lfd, (struct sockaddr *)&addr, sizeof addr) != 0 ||
        listen(lfd, 4) != 0 ||
        getsockname(lfd, (struct sockaddr *)&addr, &(socklen_t){sizeof addr}) !=
            0) {
        close(lfd);
        return -1;
    }
    *port = ntohs(addr.sin_port);

    pid_t pid = fork();
    if (pid != 0) {
        close(lfd);
        return pid;
    }

    int cfd = accept(lfd, NULL, NULL);
    if (cfd < 0) {
        _exit(1);
    }
    char buf[16384];
    buf[0] = '\0';
    (void)read_request(cfd, buf, sizeof buf);

    static const char head[] =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/event-stream\r\n"
        "Connection: close\r\n"
        "\r\n"
        "data: {\"choices\":[{\"delta\":{\"content\":\"ANFANG\"}}]}\n\n";
    static const char tail[] =
        "data: {\"choices\":[{\"delta\":{\"content\":\"-ENDE\"}}]}\n\n"
        "data: [DONE]\n\n";
    (void)write(cfd, head, sizeof head - 1);
    sleep_ms(600); /* in dieser pause schlaegt der abbruch zu */
    (void)write(cfd, tail, sizeof tail - 1);
    close(cfd);
    _exit(0);
}

/* redraw-zaehler statt echtem draw() */
static int g_redraws = 0;

static void *g_redraw_ctx = NULL;

static void count_redraw(void *ud)
{
    g_redraws++;
    g_redraw_ctx = ud; /* wird der ctx durchgereicht? siehe unten */
}

/* standard-haken der tests: nur zeichnen – tools laufen ohnehin
 * ungefragt durch (der hook existiert nicht mehr) */
static const SendHooks HOOKS = {.redraw = count_redraw};

/* ------------------------------------------------------------------ */
/* agent-loop: zwei runden gegen einen routing-mock. runde 1 (request */
/* enthaelt noch kein tool_call_id) antwortet mit einem tool-call,    */
/* runde 2 bekommt die finale antwort. das tool laeuft dazwischen     */
/* WIRKLICH (echo ueber popen).                                        */
/* ------------------------------------------------------------------ */

static pid_t start_tool_server(int *port)
{
    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    if (lfd < 0) {
        return -1;
    }
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (bind(lfd, (struct sockaddr *)&addr, sizeof addr) != 0 ||
        listen(lfd, 4) != 0 ||
        getsockname(lfd, (struct sockaddr *)&addr, &(socklen_t){sizeof addr}) !=
            0) {
        close(lfd);
        return -1;
    }
    *port = ntohs(addr.sin_port);

    pid_t pid = fork();
    if (pid != 0) {
        close(lfd);
        return pid;
    }

    /* child: zwei verbindungen, routing nach request-inhalt */
    for (int round = 0; round < 2; round++) {
        int cfd = accept(lfd, NULL, NULL);
        if (cfd < 0) {
            _exit(1);
        }
        char req[16384];
        req[0] = '\0';
        (void)read_request(cfd, req, sizeof req);

        const char *resp = NULL;
        if (strstr(req, "tool_call_id") != NULL) {
            /* runde 2: finale antwort (das tool-ergebnis ist im
             * request angekommen) */
            resp =
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: text/event-stream\r\n"
                "Connection: close\r\n"
                "\r\n"
                "data: {\"choices\":[{\"delta\":{\"content\":\"fertig: "
                "\"}}]}\n\n"
                "data: "
                "{\"choices\":[{\"delta\":{\"content\":\"agent-test\"}}]}\n\n"
                "data: [DONE]\n\n";
        } else {
            /* runde 1: ein tool-call (bash echo agent-test) */
            resp = "HTTP/1.1 200 OK\r\n"
                   "Content-Type: text/event-stream\r\n"
                   "Connection: close\r\n"
                   "\r\n"
                   "data: {\"choices\":[{\"delta\":{\"tool_calls\":["
                   "{\"index\":0,\"id\":\"call_1\",\"function\":{"
                   "\"name\":\"bash\",\"arguments\":"
                   "\"{\\\"command\\\":\\\"touch " CONFIRM_MARK
                   " && echo agent-test\\\"}\"}}]}}]}\n\n"
                   "data: [DONE]\n\n";
        }
        (void)write(cfd, resp, strlen(resp));
        close(cfd);
    }
    _exit(0);
}

/* server fuer den parallel-test: runde 1 liefert ZWEI bash-calls
 * (je 0.4 s sleep), runde 2 die finale antwort. sind die calls
 * parallel gelaufen, dauert die runde ~0.4 s statt ~0.8 s. */
static pid_t start_parallel_server(int *port)
{
    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    if (lfd < 0) {
        return -1;
    }
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (bind(lfd, (struct sockaddr *)&addr, sizeof addr) != 0 ||
        listen(lfd, 4) != 0 ||
        getsockname(lfd, (struct sockaddr *)&addr, &(socklen_t){sizeof addr}) !=
            0) {
        close(lfd);
        return -1;
    }
    *port = ntohs(addr.sin_port);

    pid_t pid = fork();
    if (pid != 0) {
        close(lfd);
        return pid;
    }

    for (int round = 0; round < 2; round++) {
        int cfd = accept(lfd, NULL, NULL);
        if (cfd < 0) {
            _exit(1);
        }
        char req[65536];
        req[0] = '\0';
        (void)read_request(cfd, req, sizeof req);
        const char *resp = NULL;
        if (round == 1) {
            resp = "HTTP/1.1 200 OK\r\n"
                   "Content-Type: text/event-stream\r\n"
                   "Connection: close\r\n"
                   "\r\n"
                   "data: {\"choices\":[{\"delta\":{\"content\":\"beide "
                   "da\"}}]}\n\n"
                   "data: [DONE]\n\n";
        } else {
            resp = "HTTP/1.1 200 OK\r\n"
                   "Content-Type: text/event-stream\r\n"
                   "Connection: close\r\n"
                   "\r\n"
                   "data: {\"choices\":[{\"delta\":{\"tool_calls\":["
                   "{\"index\":0,\"id\":\"call_a\",\"function\":{"
                   "\"name\":\"bash\",\"arguments\":"
                   "\"{\\\"command\\\":\\\"sleep 0.4; echo a\\\"}\"}},"
                   "{\"index\":1,\"id\":\"call_b\",\"function\":{"
                   "\"name\":\"bash\",\"arguments\":"
                   "\"{\\\"command\\\":\\\"sleep 0.4; echo b\\\"}\"}}"
                   "]}}]}\n\n"
                   "data: [DONE]\n\n";
        }
        (void)write(cfd, resp, strlen(resp));
        close(cfd);
    }
    _exit(0);
}

/* chunked-response schreiben und den socket OFFEN halten: erst
 * so kann ein mock mehrere requests auf EINER verbindung
 * bedienen (SSE endet normal am connection-close, das hier
 * explizit NICHT gesendet wird). */
static void write_chunked_keepalive(int cfd, const char *body)
{
    char head[256];
    int n = snprintf(head, sizeof head,
                     "HTTP/1.1 200 OK\r\n"
                     "Content-Type: text/event-stream\r\n"
                     "Transfer-Encoding: chunked\r\n"
                     "\r\n"
                     "%zx\r\n",
                     strlen(body));
    (void)write(cfd, head, (size_t)n);
    (void)write(cfd, body, strlen(body));
    (void)write(cfd, "\r\n0\r\n\r\n", 7);
}

/* client-reuse: dieser server akzeptiert GENAU EINE verbindung und
 * bedient beide anfragen des agent-loops (runde 1: tool-call,
 * runde 2: finale antwort) auf demselben socket. wuerde send_stream
 * pro runde einen neuen client bauen, ginge die zweite anfrage auf
 * eine neue verbindung – die akzeptiert der server spaeter nur noch
 * als bogus und beantwortet sie mit 500, damit der test schnell
 * faelschlich failt statt am 120s-timeout zu haengen. */
static pid_t start_keepalive_server(int *port)
{
    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    if (lfd < 0) {
        return -1;
    }
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (bind(lfd, (struct sockaddr *)&addr, sizeof addr) != 0 ||
        listen(lfd, 4) != 0 ||
        getsockname(lfd, (struct sockaddr *)&addr, &(socklen_t){sizeof addr}) !=
            0) {
        close(lfd);
        return -1;
    }
    *port = ntohs(addr.sin_port);

    pid_t pid = fork();
    if (pid != 0) {
        close(lfd);
        return pid;
    }

    int cfd = accept(lfd, NULL, NULL);
    if (cfd < 0) {
        _exit(1);
    }
    char req[65536];
    for (int round = 0; round < 2; round++) {
        req[0] = '\0';
        (void)read_request(cfd, req, sizeof req);
        if (round == 0) {
            write_chunked_keepalive(
                cfd, "data: {\"choices\":[{\"delta\":{\"tool_calls\":["
                     "{\"index\":0,\"id\":\"call_1\",\"function\":{"
                     "\"name\":\"bash\",\"arguments\":"
                     "\"{\\\"command\\\":\\\"echo reuse-test\\\"}\"}}"
                     "]}}]}\n\n"
                     "data: [DONE]\n\n");
        } else {
            write_chunked_keepalive(
                cfd, "data: {\"choices\":[{\"delta\":{\"content\":"
                     "\"auf einer verbindung\"}}]}\n\n"
                     "data: [DONE]\n\n");
        }
    }
    close(cfd);

    /* eine dritte verbindung waere der reuse-fehler: schnell 500 */
    int bogus = accept(lfd, NULL, NULL);
    if (bogus >= 0) {
        static const char err[] =
            "HTTP/1.1 500 Connection wurde nicht wiederverwendet\r\n"
            "Content-Length: 0\r\n"
            "\r\n";
        (void)write(bogus, err, sizeof err - 1);
        close(bogus);
    }
    _exit(0);
}

/* ein provider, ein modell, base-url auf den mock-port */
static void build_mock_cfg(Config *cfg, int port)
{
    char url[64];
    snprintf(url, sizeof url, "http://127.0.0.1:%d/v1", port);

    memset(cfg, 0, sizeof *cfg);
    cfg->providers = calloc(1, sizeof(Provider));
    if (cfg->providers == NULL) {
        die("out of memory");
    }
    cfg->providers_len = 1;
    cfg->providers[0].api_key = dup_str("x");
    cfg->providers[0].base_url = dup_str(url);
    cfg->providers[0].models = calloc(1, sizeof(Model));
    if (cfg->providers[0].models == NULL) {
        die("out of memory");
    }
    cfg->providers[0].models_len = 1;
    cfg->providers[0].models[0].id = dup_str("mock");
    cfg->active_model = dup_str("mock");
    CHECK(cfg->providers[0].api_key != NULL);
    CHECK(cfg->providers[0].base_url != NULL);
    CHECK(cfg->providers[0].models[0].id != NULL);
    CHECK(cfg->active_model != NULL);
}

static void free_mock_cfg(Config *cfg)
{
    free(cfg->providers[0].api_key);
    free(cfg->providers[0].base_url);
    free(cfg->providers[0].models[0].id);
    free(cfg->providers[0].models);
    free(cfg->active_model);
    free(cfg->providers);
}

static void test_agent(void)
{
    int port = 0;
    pid_t server = start_tool_server(&port);
    CHECK(server >= 0);
    if (server < 0) {
        return;
    }
    Config cfg;
    build_mock_cfg(&cfg, port);

    AppState st = {0};
    input_init(&st.input);
    CHECK(chat_append(&st.chat, CHAT_ROLE_USER, "tu was") == 0);

    g_redraws = 0;
    int rc = send_stream(&st, &cfg, &HOOKS);
    CHECK(rc == 0);

    /* verlauf: user -> assistant mit tool-call -> tool-ergebnis
     * -> finale antwort */
    CHECK(st.chat.len == 4);
    CHECK(st.chat.msgs[1].role == CHAT_ROLE_ASSISTANT);
    CHECK(st.chat.msgs[1].tool_calls_len == 1);
    CHECK(st.chat.msgs[1].tool_calls[0].name != NULL &&
          strcmp(st.chat.msgs[1].tool_calls[0].name, "bash") == 0);
    CHECK(st.chat.msgs[1].tool_calls[0].arguments != NULL &&
          strcmp(st.chat.msgs[1].tool_calls[0].arguments,
                 "{\"command\":\"touch " CONFIRM_MARK
                 " && echo agent-test\"}") == 0);
    CHECK(st.chat.msgs[2].role == CHAT_ROLE_TOOL);
    CHECK(st.chat.msgs[2].text != NULL &&
          strstr(st.chat.msgs[2].text, "agent-test") != NULL);
    CHECK(st.chat.msgs[2].tool_call_id != NULL &&
          strcmp(st.chat.msgs[2].tool_call_id, "call_1") == 0);
    CHECK(st.chat.msgs[3].role == CHAT_ROLE_ASSISTANT);
    CHECK(st.chat.msgs[3].text != NULL);
    CHECK(strcmp(st.chat.msgs[3].text, "fertig: agent-test") == 0);

    chat_free(&st.chat);
    input_free(&st.input);

    free_mock_cfg(&cfg);

    kill(server, SIGKILL);
    waitpid(server, NULL, 0);
}

/* ------------------------------------------------------------------ */
/* kaputte modell-antwort: ein tool-call-delta traegt eine id, aber   */
/* keinen funktionsnamen. der akku muss den slot verwerfen und darf   */
/* die id danach nicht ein zweites mal freigeben (asan faengt das).   */
/* ------------------------------------------------------------------ */

static void test_agent_broken_call(void)
{
    static const char resp[] =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/event-stream\r\n"
        "Connection: close\r\n"
        "\r\n"
        "data: {\"choices\":[{\"delta\":{\"tool_calls\":["
        "{\"index\":0,\"id\":\"call_ohne_namen\","
        "\"function\":{\"arguments\":\"{}\"}}]}}]}\n\n"
        "data: [DONE]\n\n";

    int port = 0;
    pid_t server = start_once_server(&port, resp);
    CHECK(server >= 0);
    if (server < 0) {
        return;
    }

    Config cfg;
    build_mock_cfg(&cfg, port);

    AppState st = {0};
    input_init(&st.input);
    CHECK(chat_append(&st.chat, CHAT_ROLE_USER, "tu was") == 0);

    g_redraws = 0;
    int rc = send_stream(&st, &cfg, &HOOKS);
    CHECK(rc == 0);

    /* namenloser call verworfen -> keine tool-runde, loop endet */
    CHECK(st.chat.len == 2);
    CHECK(st.chat.msgs[1].role == CHAT_ROLE_ASSISTANT);
    CHECK(st.chat.msgs[1].tool_calls_len == 0);
    CHECK(st.chat.msgs[1].tool_calls == NULL);

    chat_free(&st.chat);
    input_free(&st.input);
    free_mock_cfg(&cfg);

    kill(server, SIGKILL);
    waitpid(server, NULL, 0);
}

/* ------------------------------------------------------------------ */
/* context-management: laeuft das fenster ueber, wird der abfallende */
/* verlauf per compaction zu einer summary verdichtet (llm-call) –  */
/* der alte block geht NICHT mehr wortwoertlich raus, aber sein    */
/* inhalt lebt in der summary weiter, und der benutzer sieht die    */
/* compaction im verlauf.                                            */
/* ------------------------------------------------------------------ */

static void test_context_compaction(void)
{
    int port = 0;
    pid_t server = start_compact_probe_server(&port, "URALTE-NACHRICHT",
                                              "## Goal: zusammenfassung-test");
    CHECK(server >= 0);
    if (server < 0) {
        return;
    }

    Config cfg;
    build_mock_cfg(&cfg, port);
    /* kleines fenster: der alte block passt garantiert nicht mehr */
    cfg.providers[0].models[0].context_window = 2000;

    AppState st = {0};
    input_init(&st.input);

    /* ~8 kb alter verlauf (~2000 tokens) + eine kurze neue frage */
    char *old = malloc(8193);
    if (old == NULL) {
        die("out of memory");
    }
    memset(old, 'x', 8192);
    old[8192] = '\0';
    memcpy(old, "URALTE-NACHRICHT ", strlen("URALTE-NACHRICHT "));
    CHECK(chat_append(&st.chat, CHAT_ROLE_USER, old) == 0);
    free(old);
    CHECK(chat_append(&st.chat, CHAT_ROLE_USER, "neue frage") == 0);

    g_redraws = 0;
    int rc = send_stream(&st, &cfg, &HOOKS);
    CHECK(rc == 0);

    /* der server hat den alten block wortwoertlich NICHT gesehen
     * (er steckt nur in der summary) */
    ChatMessage *answer = &st.chat.msgs[st.chat.len - 1];
    CHECK(answer->role == CHAT_ROLE_ASSISTANT);
    CHECK(answer->text != NULL && strcmp(answer->text, "NEIN") == 0);

    /* compaction: summary uebernommen, watermark hinter dem alten
     * block, kuerzung im verlauf vermerkt */
    CHECK(st.ctx.dropped == 1);
    CHECK(st.ctx.summary != NULL);
    CHECK(strcmp(st.ctx.summary, "## Goal: zusammenfassung-test") == 0);
    CHECK(st.ctx.covered == 1);
    CHECK(st.ctx.compact_failed == false);
    bool has_notice = false;
    for (size_t i = 0; i < st.chat.len; i++) {
        if (st.chat.msgs[i].role == CHAT_ROLE_NOTICE &&
            strstr(st.chat.msgs[i].text, "verlauf komprimiert") != NULL &&
            strstr(st.chat.msgs[i].text, "wird") == NULL) {
            has_notice = true; /* die fertige compaction, nicht die
                                * vorankuendigung davor */
        }
    }
    CHECK(has_notice);

    /* usage aus dem stream hat die schaetzung geeicht */
    CHECK(st.ctx.prompt_tokens == 1234);
    CHECK(st.ctx.scale > 0);
    CHECK(st.ctx.estimated > 0);

    chat_free(&st.chat);
    ctx_reset(&st.ctx);
    input_free(&st.input);
    free_mock_cfg(&cfg);

    kill(server, SIGKILL);
    waitpid(server, NULL, 0);
}

/* faellt die compaction aus (server weg), bleibt trimming der
 * fallback: alter block weg, hinweis im verlauf, runde lauft */
static void test_context_compaction_fallback(void)
{
    int port = 0;
    pid_t server = start_compact_probe_server(&port, "URALTE-NACHRICHT",
                                              "## Goal: zusammenfassung-test");
    CHECK(server >= 0);
    if (server < 0) {
        return;
    }

    Config cfg;
    build_mock_cfg(&cfg, port);
    cfg.providers[0].models[0].context_window = 2000;

    AppState st = {0};
    input_init(&st.input);

    char *old = malloc(8193);
    if (old == NULL) {
        die("out of memory");
    }
    memset(old, 'x', 8192);
    old[8192] = '\0';
    memcpy(old, "URALTE-NACHRICHT ", strlen("URALTE-NACHRICHT "));
    CHECK(chat_append(&st.chat, CHAT_ROLE_USER, old) == 0);
    free(old);
    CHECK(chat_append(&st.chat, CHAT_ROLE_USER, "neue frage") == 0);

    /* compaction-server gleich wieder killen: der erste request
     * (compaction) scheitert an der verbindung, der zweite (die
     * eigentliche antwort) auch – die runde endet mit fehler */
    kill(server, SIGKILL);
    waitpid(server, NULL, 0);

    int rc = send_stream(&st, &cfg, &HOOKS);
    CHECK(rc == -1);

    /* fallback: summary frei, kuerzung vermerkt, retry-flag gesetzt */
    CHECK(st.ctx.summary == NULL);
    CHECK(st.ctx.compact_failed == true);
    CHECK(st.ctx.covered == st.ctx.dropped);
    bool has_notice = false;
    for (size_t i = 0; i < st.chat.len; i++) {
        if (st.chat.msgs[i].role == CHAT_ROLE_NOTICE &&
            strstr(st.chat.msgs[i].text, "compaction fehlgeschlagen") != NULL) {
            has_notice = true;
        }
    }
    CHECK(has_notice);

    chat_free(&st.chat);
    ctx_reset(&st.ctx);
    input_free(&st.input);
    free_mock_cfg(&cfg);
}

/* ------------------------------------------------------------------ */

/* /compact: die manuelle compaction verdichtet den verlauf bis auf
 * das keep-fenster, ohne auch nur eine nachricht wegzulassen. der
 * mock-server antwortet auf die compaction mit einer festen summary
 * (verbinding 1); die zweite verbindung (probe) kommt nie – der
 * server wird danach gekillt. */
static void test_compact_now(void)
{
    int port = 0;
    pid_t server = start_compact_probe_server(&port, "URALTE-NACHRICHT",
                                              "## Goal: manuelle-compaction");
    CHECK(server >= 0);
    if (server < 0) {
        return;
    }

    Config cfg;
    build_mock_cfg(&cfg, port);

    AppState st = {0};
    input_init(&st.input);

    /* leerer verlauf: notice, kein request an den server */
    CHECK(send_compact_now(&st, &cfg, &HOOKS) == 0);
    bool empty_notice = false;
    for (size_t i = 0; i < st.chat.len; i++) {
        if (st.chat.msgs[i].role == CHAT_ROLE_NOTICE &&
            strstr(st.chat.msgs[i].text, "nichts zu komprimieren") != NULL) {
            empty_notice = true;
        }
    }
    CHECK(empty_notice);

    /* ~104 kb verlauf: > keep-fenster (20k tokens ~ 80 kb), also
     * gibt es etwas zu verdichten */
    char *old = malloc(8193);
    if (old == NULL) {
        die("out of memory");
    }
    memset(old, 'x', 8192);
    old[8192] = '\0';
    memcpy(old, "URALTE-NACHRICHT ", strlen("URALTE-NACHRICHT "));
    size_t first_msg = st.chat.len; /* notices stehen schon davor */
    for (int i = 0; i < 13; i++) {
        CHECK(chat_append(&st.chat, CHAT_ROLE_USER, old) == 0);
    }
    free(old);
    CHECK(chat_append(&st.chat, CHAT_ROLE_USER, "neue frage") == 0);
    size_t len_before = st.chat.len;
    CHECK(len_before == first_msg + 14);

    g_redraws = 0;
    CHECK(send_compact_now(&st, &cfg, &HOOKS) == 0);

    /* summary uebernommen, watermark mitten im verlauf – und keine
     * nachricht ist verschwunden (manuell wird nichts weggeworfen,
     * anders als beim fenster-ueberlauf; die notices der app haengen
     * natuerlich selbst hinten dran) */
    CHECK(st.ctx.summary != NULL);
    CHECK(strcmp(st.ctx.summary, "## Goal: manuelle-compaction") == 0);
    CHECK(st.ctx.covered > 0 && st.ctx.covered < len_before);
    CHECK(st.chat.len >= len_before);
    CHECK(strncmp(st.chat.msgs[first_msg].text, "URALTE-NACHRICHT", 16) == 0);
    CHECK(strcmp(st.chat.msgs[len_before - 1].text, "neue frage") == 0);
    CHECK(st.ctx.compact_failed == false);
    bool done_notice = false;
    for (size_t i = 0; i < st.chat.len; i++) {
        if (st.chat.msgs[i].role == CHAT_ROLE_NOTICE &&
            strstr(st.chat.msgs[i].text, "verlauf komprimiert") != NULL &&
            strstr(st.chat.msgs[i].text, "wird") == NULL) {
            done_notice = true;
        }
    }
    CHECK(done_notice);

    /* der compaction-request enthaelt den alten block (der server
     * haette sonst nichts zu verdichten); die summary landet als
     * USER-nachricht im request, wenn der naechste turn laeuft */
    char *wrapped = ctx_summary_wrap(st.ctx.summary);
    CHECK(wrapped != NULL);
    OaiMessage *msgs = NULL;
    int n = send_build_messages_from(&st.chat, st.ctx.covered, "sys", wrapped,
                                     &msgs);
    CHECK(n > 2);
    CHECK(msgs != NULL && msgs[1].role == OAI_ROLE_USER);
    CHECK(strstr(msgs[1].content, "<summary>") != NULL);
    free(msgs);
    free(wrapped);

    /* zweiter aufruf: dieser teil ist bereits komprimiert – keine
     * neue summary, derselbe zustand */
    CHECK(send_compact_now(&st, &cfg, &HOOKS) == 0);
    CHECK(st.ctx.summary != NULL &&
          strcmp(st.ctx.summary, "## Goal: manuelle-compaction") == 0);
    bool again_notice = false;
    for (size_t i = 0; i < st.chat.len; i++) {
        if (st.chat.msgs[i].role == CHAT_ROLE_NOTICE &&
            strstr(st.chat.msgs[i].text, "bereits") != NULL) {
            again_notice = true;
        }
    }
    CHECK(again_notice);

    chat_free(&st.chat);
    ctx_reset(&st.ctx);
    input_free(&st.input);
    free_mock_cfg(&cfg);
    kill(server, SIGKILL);
    waitpid(server, NULL, 0);
}

/* gegenprobe: grosses fenster -> nichts wird weggelassen */
static void test_context_fits(void)
{
    int port = 0;
    pid_t server = start_probe_server(&port, "URALTE-NACHRICHT");
    CHECK(server >= 0);
    if (server < 0) {
        return;
    }

    Config cfg;
    build_mock_cfg(&cfg, port);
    cfg.providers[0].models[0].context_window = 200000;

    AppState st = {0};
    input_init(&st.input);
    CHECK(chat_append(&st.chat, CHAT_ROLE_USER, "URALTE-NACHRICHT hallo") == 0);
    CHECK(chat_append(&st.chat, CHAT_ROLE_USER, "neue frage") == 0);

    CHECK(send_stream(&st, &cfg, &HOOKS) == 0);
    ChatMessage *answer = &st.chat.msgs[st.chat.len - 1];
    CHECK(answer->text != NULL && strcmp(answer->text, "JA") == 0);
    CHECK(st.ctx.dropped == 0);
    for (size_t i = 0; i < st.chat.len; i++) {
        CHECK(st.chat.msgs[i].role != CHAT_ROLE_NOTICE);
    }

    chat_free(&st.chat);
    input_free(&st.input);
    free_mock_cfg(&cfg);

    kill(server, SIGKILL);
    waitpid(server, NULL, 0);
}

/* ------------------------------------------------------------------ */
/* abbruch: esc/ctrl+c waehrend der antwort stoppt den stream. was    */
/* schon da ist, bleibt stehen – der rest kommt nie an.               */
/* ------------------------------------------------------------------ */

static void test_stream_abort(void)
{
    int port = 0;
    pid_t server = start_slow_server(&port);
    CHECK(server >= 0);
    if (server < 0) {
        return;
    }

    Config cfg;
    build_mock_cfg(&cfg, port);

    AppState st = {0};
    input_init(&st.input);
    CHECK(chat_append(&st.chat, CHAT_ROLE_USER, "erzaehl was langes") == 0);

    /* ctrl+c liegt an, bevor der stream laeuft: der watchdog des
     * clients findet es beim ersten tick */
    keys_unread("\x03", 1);

    g_redraws = 0;
    struct timespec t0;
    struct timespec t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    int rc = send_stream(&st, &cfg, &HOOKS);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    CHECK(rc == 0); /* abbruch ist kein fehler */

    /* der server pausiert 600ms vor dem rest. sind wir deutlich
     * frueher zurueck, wurde wirklich abgebrochen und nicht nur
     * hinterher ein hinweis gesetzt. */
    long ms = ((t1.tv_sec - t0.tv_sec) * 1000) +
              ((t1.tv_nsec - t0.tv_nsec) / 1000000);
    CHECK(ms < 500);

    /* der erste teil steht im verlauf, der zweite kam nie an */
    bool has_notice = false;
    bool has_part = false;
    for (size_t i = 0; i < st.chat.len; i++) {
        const ChatMessage *m = &st.chat.msgs[i];
        if (m->role == CHAT_ROLE_NOTICE) {
            has_notice = true;
            CHECK(strstr(m->text, "abgebrochen") != NULL);
        }
        if (m->role == CHAT_ROLE_ASSISTANT) {
            has_part = true;
            CHECK(strstr(m->text, "-ENDE") == NULL);
        }
    }
    CHECK(has_notice);
    /* has_part haengt am timing: kam der erste chunk noch durch,
     * bleibt er stehen; kam er nicht, geht der leere platzhalter
     * weg und nur der hinweis bleibt. beides ist richtig. */
    (void)has_part;

    chat_free(&st.chat);
    input_free(&st.input);
    free_mock_cfg(&cfg);

    kill(server, SIGKILL);
    waitpid(server, NULL, 0);
}

/* gegenprobe: ohne tastendruck laeuft derselbe langsame stream durch */
static void test_stream_no_abort(void)
{
    int port = 0;
    pid_t server = start_slow_server(&port);
    CHECK(server >= 0);
    if (server < 0) {
        return;
    }

    Config cfg;
    build_mock_cfg(&cfg, port);

    AppState st = {0};
    input_init(&st.input);
    CHECK(chat_append(&st.chat, CHAT_ROLE_USER, "erzaehl was langes") == 0);

    CHECK(send_stream(&st, &cfg, &HOOKS) == 0);

    ChatMessage *answer = &st.chat.msgs[st.chat.len - 1];
    CHECK(answer->role == CHAT_ROLE_ASSISTANT);
    CHECK(answer->text != NULL && strcmp(answer->text, "ANFANG-ENDE") == 0);
    for (size_t i = 0; i < st.chat.len; i++) {
        CHECK(st.chat.msgs[i].role != CHAT_ROLE_NOTICE);
    }

    chat_free(&st.chat);
    input_free(&st.input);
    free_mock_cfg(&cfg);

    kill(server, SIGKILL);
    waitpid(server, NULL, 0);
}

/* ------------------------------------------------------------------ */
/* regression: im agent-loop zeigte ein ChatMessage* in das msgs-     */
/* array, waehrend chat_append_tool() es wachsen liess. mehrere       */
/* tool-calls in EINER antwort loesen das realloc mitten in der       */
/* schleife aus – ohne den fix meldet asan hier use-after-free.       */
/* ------------------------------------------------------------------ */

static pid_t start_multitool_server(int *port)
{
    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    if (lfd < 0) {
        return -1;
    }
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (bind(lfd, (struct sockaddr *)&addr, sizeof addr) != 0 ||
        listen(lfd, 4) != 0 ||
        getsockname(lfd, (struct sockaddr *)&addr, &(socklen_t){sizeof addr}) !=
            0) {
        close(lfd);
        return -1;
    }
    *port = ntohs(addr.sin_port);

    pid_t pid = fork();
    if (pid != 0) {
        close(lfd);
        return pid;
    }

    for (int round = 0; round < 2; round++) {
        int cfd = accept(lfd, NULL, NULL);
        if (cfd < 0) {
            _exit(1);
        }
        char *req = malloc(1 << 20);
        if (req == NULL) {
            _exit(1);
        }
        req[0] = '\0';
        (void)read_request(cfd, req, 1 << 20);
        bool second = strstr(req, "tool_call_id") != NULL;
        free(req);

        const char *resp = NULL;
        if (second) {
            resp = "HTTP/1.1 200 OK\r\n"
                   "Content-Type: text/event-stream\r\n"
                   "Connection: close\r\n"
                   "\r\n"
                   "data: {\"choices\":[{\"delta\":{\"content\":\"fertig\"}}]}"
                   "\n\n"
                   "data: [DONE]\n\n";
        } else {
            /* runde 1: VIER calls auf einmal */
            resp = "HTTP/1.1 200 OK\r\n"
                   "Content-Type: text/event-stream\r\n"
                   "Connection: close\r\n"
                   "\r\n"
                   "data: {\"choices\":[{\"delta\":{\"tool_calls\":["
                   "{\"index\":0,\"id\":\"c0\",\"function\":{\"name\":"
                   "\"bash\",\"arguments\":\"{\\\"command\\\":"
                   "\\\"echo a\\\"}\"}},"
                   "{\"index\":1,\"id\":\"c1\",\"function\":{\"name\":"
                   "\"bash\",\"arguments\":\"{\\\"command\\\":"
                   "\\\"echo b\\\"}\"}},"
                   "{\"index\":2,\"id\":\"c2\",\"function\":{\"name\":"
                   "\"bash\",\"arguments\":\"{\\\"command\\\":"
                   "\\\"echo c\\\"}\"}},"
                   "{\"index\":3,\"id\":\"c3\",\"function\":{\"name\":"
                   "\"bash\",\"arguments\":\"{\\\"command\\\":"
                   "\\\"echo d\\\"}\"}}]}}]}\n\n"
                   "data: [DONE]\n\n";
        }
        (void)write(cfd, resp, strlen(resp));
        close(cfd);
    }
    _exit(0);
}

static void test_agent_realloc(void)
{
    int port = 0;
    pid_t server = start_multitool_server(&port);
    CHECK(server >= 0);
    if (server < 0) {
        return;
    }

    Config cfg;
    build_mock_cfg(&cfg, port);

    AppState st = {0};
    input_init(&st.input);

    /* den verlauf so fuellen, dass die naechsten anhaenge-vorgaenge
     * die kapazitaet sprengen (chat waechst 8 -> 16 -> ...) */
    for (int i = 0; i < 6; i++) {
        CHECK(chat_append(&st.chat, CHAT_ROLE_USER, "fuellnachricht") == 0);
        CHECK(chat_append(&st.chat, CHAT_ROLE_ASSISTANT, "ok") == 0);
    }
    CHECK(chat_append(&st.chat, CHAT_ROLE_USER, "tu vier dinge") == 0);

    CHECK(send_stream(&st, &cfg, &HOOKS) == 0);

    /* alle vier tool-ergebnisse sind im verlauf gelandet */
    size_t tools = 0;
    for (size_t i = 0; i < st.chat.len; i++) {
        if (st.chat.msgs[i].role == CHAT_ROLE_TOOL) {
            tools++;
            CHECK(st.chat.msgs[i].tool_call_id != NULL);
        }
    }
    CHECK(tools == 4);
    CHECK(strcmp(st.chat.msgs[st.chat.len - 1].text, "fertig") == 0);

    chat_free(&st.chat);
    input_free(&st.input);
    free_mock_cfg(&cfg);

    kill(server, SIGKILL);
    waitpid(server, NULL, 0);
}

/* der ctx aus SendHooks muss unveraendert im redraw-callback
 * ankommen. das klingt trivial, ist aber die stelle, an der die UI
 * ihren zeichen-kontext bekommt: geht sie verloren, dereferenziert
 * stream_redraw NULL – und zwar erst im echten betrieb, weil die
 * uebrigen tests mit ctx == NULL fahren. */
/* zwei parallel laufende bash-calls dauern so lange wie EINER:
 * sequenziell waeren es mindestens 0.8 s sleep. misst die
 * gesamtzeit des turns und haelt sie gegen den grenzwert. */
static void test_tools_parallel(void)
{
    int port = 0;
    pid_t server = start_parallel_server(&port);
    CHECK(server >= 0);
    if (server < 0) {
        return;
    }

    Config cfg;
    build_mock_cfg(&cfg, port);

    AppState st = {0};
    input_init(&st.input);
    CHECK(chat_append(&st.chat, CHAT_ROLE_USER, "mach zwei dinge") == 0);

    long long t0 = mono_ms();
    int rc = send_stream(&st, &cfg, &HOOKS);
    long long dt = mono_ms() - t0;
    CHECK(rc == 0);

    /* beide ergebnisse da, in call-reihenfolge; danach die finale
     * antwort: user, assistant(2 calls), tool, tool, assistant */
    CHECK(st.chat.len == 5);
    CHECK(st.chat.msgs[1].tool_calls_len == 2);
    CHECK(st.chat.msgs[2].role == CHAT_ROLE_TOOL);
    CHECK(st.chat.msgs[2].tool_call_id != NULL &&
          strcmp(st.chat.msgs[2].tool_call_id, "call_a") == 0);
    CHECK(strstr(st.chat.msgs[2].text, "a\n") != NULL);
    CHECK(st.chat.msgs[3].role == CHAT_ROLE_TOOL);
    CHECK(st.chat.msgs[3].tool_call_id != NULL &&
          strcmp(st.chat.msgs[3].tool_call_id, "call_b") == 0);
    CHECK(strstr(st.chat.msgs[3].text, "b\n") != NULL);
    CHECK(strcmp(st.chat.msgs[4].text, "beide da") == 0);

    /* parallel: gut unter 2 * 400 ms sleep + overhead. sequenziell
     * waeren es 800 ms sleep ALLEIN – der grenzwert liegt mit
     * 750 ms dazwischen */
    CHECK(dt < 750);
    CHECK(dt >= 400);

    chat_free(&st.chat);
    input_free(&st.input);
    free_mock_cfg(&cfg);

    kill(server, SIGKILL);
    waitpid(server, NULL, 0);
}

/* beide anfragen eines agent-loops (tool-runde + antwort-runde)
 * laufen auf EINER verbindung: der client bleibt ueber die runden
 * am leben, kein tcp/tls-handshake pro runde. */
static void test_client_reuse(void)
{
    int port = 0;
    pid_t server = start_keepalive_server(&port);
    CHECK(server >= 0);
    if (server < 0) {
        return;
    }

    Config cfg;
    build_mock_cfg(&cfg, port);

    AppState st = {0};
    input_init(&st.input);
    CHECK(chat_append(&st.chat, CHAT_ROLE_USER, "frage") == 0);

    int rc = send_stream(&st, &cfg, &HOOKS);
    CHECK(rc == 0);

    /* runde 1: tool-call ausgefuehrt; runde 2: finale antwort */
    CHECK(st.chat.len == 4); /* user, assistant(call), tool, assistant */
    CHECK(strstr(st.chat.msgs[2].text, "reuse-test") != NULL);
    CHECK(strcmp(st.chat.msgs[3].text, "auf einer verbindung") == 0);

    chat_free(&st.chat);
    input_free(&st.input);
    free_mock_cfg(&cfg);

    kill(server, SIGKILL);
    waitpid(server, NULL, 0);
}

static void test_hooks_ctx(void)
{
    int port = 0;
    pid_t server = start_sse_server(&port);
    CHECK(server >= 0);
    if (server < 0) {
        return;
    }

    Config cfg;
    build_mock_cfg(&cfg, port);

    AppState st = {0};
    input_init(&st.input);
    CHECK(chat_append(&st.chat, CHAT_ROLE_USER, "hallo") == 0);

    int marker = 4711;
    g_redraw_ctx = NULL;
    g_redraws = 0;
    SendHooks hooks = {.ctx = &marker, .redraw = count_redraw};
    CHECK(send_stream(&st, &cfg, &hooks) == 0);

    CHECK(g_redraws > 0);
    CHECK(g_redraw_ctx == &marker); /* exakt der uebergebene zeiger */

    chat_free(&st.chat);
    input_free(&st.input);
    free_mock_cfg(&cfg);
    kill(server, SIGKILL);
    waitpid(server, NULL, 0);
}

static void test_stream(void)
{
    /* mock-server starten und eine streaming-config bauen */
    int port = 0;
    pid_t server = start_sse_server(&port);
    CHECK(server >= 0);
    if (server < 0) {
        return;
    }
    char url[64];
    snprintf(url, sizeof url, "http://127.0.0.1:%d/v1", port);

    Config cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.providers = calloc(1, sizeof(Provider));
    if (cfg.providers == NULL) {
        die("out of memory");
    }
    cfg.providers_len = 1;
    cfg.providers[0].api_key = dup_str("x");
    cfg.providers[0].base_url = dup_str(url);
    cfg.providers[0].models = calloc(1, sizeof(Model));
    if (cfg.providers[0].models == NULL) {
        die("out of memory");
    }
    cfg.providers[0].models_len = 1;
    cfg.providers[0].models[0].id = dup_str("mock");
    cfg.active_model = dup_str("mock");
    CHECK(cfg.providers[0].api_key != NULL);
    CHECK(cfg.providers[0].base_url != NULL);
    CHECK(cfg.providers[0].models[0].id != NULL);
    CHECK(cfg.active_model != NULL);

    AppState st = {0};
    input_init(&st.input);
    CHECK(chat_append(&st.chat, CHAT_ROLE_USER, "hi") == 0);
    CHECK(chat_append(&st.chat, CHAT_ROLE_ASSISTANT, "moin") == 0);

    g_redraws = 0;
    int rc = send_stream(&st, &cfg, &HOOKS);
    CHECK(rc == 0);

    /* verlauf: user, alte antwort, NEUE antwort aus den deltas */
    CHECK(st.chat.len == 3);
    CHECK(st.chat.msgs[2].role == CHAT_ROLE_ASSISTANT);
    CHECK(st.chat.msgs[2].text != NULL &&
          strcmp(st.chat.msgs[2].text, "hallo") == 0);
    /* alte nachrichten unangetastet (borrow-zeiger blieben gueltig) */
    CHECK(strcmp(st.chat.msgs[0].text, "hi") == 0);
    CHECK(strcmp(st.chat.msgs[1].text, "moin") == 0);
    /* der stream hat mindestens einmal neu gezeichnet */
    CHECK(g_redraws >= 1);

    chat_free(&st.chat);
    input_free(&st.input);

    free(cfg.providers[0].api_key);
    free(cfg.providers[0].base_url);
    free(cfg.providers[0].models[0].id);
    free(cfg.providers[0].models);
    free(cfg.active_model);
    free(cfg.providers);

    kill(server, SIGKILL);
    waitpid(server, NULL, 0);

    /* --- stream-fehler: tote adresse -> platzhalter weg, fehler hin */
    Config bad = {0};
    bad.providers = calloc(1, sizeof(Provider));
    if (bad.providers == NULL) {
        die("out of memory");
    }
    bad.providers_len = 1;
    bad.providers[0].api_key = dup_str("x");
    bad.providers[0].base_url =
        dup_str("http://127.0.0.1:1/v1"); /* port 1: refused */
    bad.providers[0].models = calloc(1, sizeof(Model));
    if (bad.providers[0].models == NULL) {
        die("out of memory");
    }
    bad.providers[0].models_len = 1;
    bad.providers[0].models[0].id = dup_str("mock");
    bad.active_model = dup_str("mock");
    CHECK(bad.providers[0].api_key != NULL);
    CHECK(bad.providers[0].base_url != NULL);
    CHECK(bad.active_model != NULL);

    AppState st2 = {0};
    input_init(&st2.input);
    CHECK(chat_append(&st2.chat, CHAT_ROLE_USER, "hi") == 0);

    rc = send_stream(&st2, &bad, &HOOKS);
    CHECK(rc == -1);
    /* platzhalter ist weg: nur user + fehlermeldung */
    CHECK(st2.chat.len == 2);
    CHECK(st2.chat.msgs[1].role == CHAT_ROLE_ERROR);
    CHECK(st2.chat.msgs[1].text != NULL &&
          strstr(st2.chat.msgs[1].text, "verbindung") != NULL);

    chat_free(&st2.chat);
    input_free(&st2.input);

    free(bad.providers[0].api_key);
    free(bad.providers[0].base_url);
    free(bad.providers[0].models[0].id);
    free(bad.providers[0].models);
    free(bad.active_model);
    free(bad.providers);
}

/* busy-queue: nach einem turn wird die gebufferte nachricht als
 * eigener turn nachgeschickt (zwei antworten auf einer verbindung,
 * der client-cache haelt sie offen). reihenfolge: erst der laufende
 * turn, dann die queue in FIFO-ordnung. */
static void test_queue_after_turn(void)
{
    int port = 0;
    pid_t server = start_two_sse_server(&port);
    CHECK(server >= 0);
    if (server < 0) {
        return;
    }

    Config cfg;
    build_mock_cfg(&cfg, port);

    AppState st = {0};
    input_init(&st.input);

    /* waehrend der ki arbeitet, puffert der benutzer zwei nach-
     * richten (der drain legt sie in die queue). hier direkt
     * eingefuellt – der drain selbst ist in test_keys getestet */
    st.queue[st.queue_n++] = dup_str("erste puffer-nachricht");
    st.queue[st.queue_n++] = dup_str("zweite");

    /* erster turn laeuft und endet: jetzt ist "naechste gelegen-
     * heit" – keys_flush_queue schickt beide als eigene turns */
    keys_flush_queue(&st, &cfg);

    /* reihenfolge: zwei user-nachrichten mit je einer antwort */
    CHECK(st.chat.len == 4);
    CHECK(st.chat.msgs[0].role == CHAT_ROLE_USER);
    CHECK(strcmp(st.chat.msgs[0].text, "erste puffer-nachricht") == 0);
    CHECK(st.chat.msgs[1].role == CHAT_ROLE_ASSISTANT);
    CHECK(st.chat.msgs[2].role == CHAT_ROLE_USER);
    CHECK(strcmp(st.chat.msgs[2].text, "zweite") == 0);
    CHECK(st.chat.msgs[3].role == CHAT_ROLE_ASSISTANT);
    CHECK(st.chat.msgs[3].text != NULL &&
          strcmp(st.chat.msgs[3].text, "ok") == 0);
    CHECK(st.queue_n == 0); /* queue ist leer */

    chat_free(&st.chat);
    input_free(&st.input);
    keys_queue_clear(&st);
    session_end(&st.session); /* send_one hat eine session geoffnet */
    free_mock_cfg(&cfg);
    kill(server, SIGKILL);
    waitpid(server, NULL, 0);
}

int main(void)
{
    /* --- send_role: mapping, ERROR ist keine api-rolle --- */
    CHECK(send_role(CHAT_ROLE_SYSTEM) == OAI_ROLE_SYSTEM);
    CHECK(send_role(CHAT_ROLE_USER) == OAI_ROLE_USER);
    CHECK(send_role(CHAT_ROLE_ASSISTANT) == OAI_ROLE_ASSISTANT);
    CHECK(send_role(CHAT_ROLE_ERROR) == -1);

    /* --- send_build_messages --- */
    Chat chat = {0};

    /* leeres chat: 0 nachrichten, *out bleibt NULL */
    OaiMessage *msgs = (void *)1;
    CHECK(send_build_messages(&chat, NULL, &msgs) == 0);
    CHECK(msgs == NULL);
    CHECK(send_build_messages(NULL, NULL, &msgs) == -1);
    CHECK(send_build_messages(&chat, NULL, NULL) == -1);

    /* leeres chat MIT system-prompt: genau die system-nachricht */
    CHECK(send_build_messages(&chat, "sys", &msgs) == 1);
    CHECK(msgs != NULL && msgs[0].role == OAI_ROLE_SYSTEM);
    CHECK(msgs != NULL && strcmp(msgs[0].content, "sys") == 0);
    free(msgs);
    msgs = NULL;

    /* nur fehler-nachrichten: ebenfalls nichts zu senden */
    CHECK(chat_append(&chat, CHAT_ROLE_ERROR, "http 500: kaputt") == 0);
    CHECK(send_build_messages(&chat, NULL, &msgs) == 0);
    CHECK(msgs == NULL);

    /* gemischtes transcript: ERROR fehlt, rest in reihenfolge */
    chat_clear(&chat);
    CHECK(chat_append(&chat, CHAT_ROLE_SYSTEM, "du bist max agent.") == 0);
    CHECK(chat_append(&chat, CHAT_ROLE_USER, "frage 1") == 0);
    CHECK(chat_append(&chat, CHAT_ROLE_ERROR, "verbindung: toter host") == 0);
    CHECK(chat_append(&chat, CHAT_ROLE_ASSISTANT, "antwort 1") == 0);
    CHECK(chat_append(&chat, CHAT_ROLE_USER, "frage 2") == 0);

    int n = send_build_messages(&chat, NULL, &msgs);
    CHECK(n == 4);
    CHECK(msgs != NULL && msgs[0].role == OAI_ROLE_SYSTEM);
    CHECK(msgs != NULL && strcmp(msgs[0].content, "du bist max agent.") == 0);
    CHECK(msgs != NULL && msgs[1].role == OAI_ROLE_USER);
    CHECK(msgs != NULL && strcmp(msgs[1].content, "frage 1") == 0);
    CHECK(msgs != NULL && msgs[2].role == OAI_ROLE_ASSISTANT); /* ERROR fehlt */
    CHECK(msgs != NULL &&
          strcmp(msgs[2].content, "antwort 1") == 0); /* rueckt nach */
    CHECK(msgs != NULL && msgs[3].role == OAI_ROLE_USER);
    CHECK(msgs != NULL && strcmp(msgs[3].content, "frage 2") == 0);

    /* content wird geborgt, nicht kopiert: pointer-gleichheit */
    CHECK(msgs != NULL && msgs[1].content == chat.msgs[1].text);
    free(msgs);
    msgs = NULL;

    /* --- tool-calls und tool-ergebnisse im request --- */
    chat_clear(&chat);
    {
        /* heap-array: chat_set_tool_calls uebernimmt den besitz */
        ChatToolCall *calls = calloc(1, sizeof *calls);
        if (calls == NULL) {
            die("out of memory");
        }
        calls[0].id = dup_str("call_1");
        calls[0].name = dup_str("bash");
        calls[0].arguments = dup_str("{\"command\":\"ls\"}");
        CHECK(calls[0].id != NULL && calls[0].name != NULL);
        CHECK(chat_append(&chat, CHAT_ROLE_ASSISTANT, "") == 0);
        CHECK(chat_set_tool_calls(&chat, calls, 1) == 0);
        CHECK(chat_append_tool(&chat, "call_1", "ergebnis") == 0);

        n = send_build_messages(&chat, NULL, &msgs);
        CHECK(n == 2);
        /* assistant mit calls: content NULL, calls geborgt */
        CHECK(msgs != NULL && msgs[0].role == OAI_ROLE_ASSISTANT);
        CHECK(msgs != NULL && msgs[0].content == NULL);
        CHECK(msgs != NULL && msgs[0].tool_calls_len == 1);
        CHECK(msgs != NULL && msgs[0].tool_calls[0].name != NULL);
        CHECK(msgs != NULL && strcmp(msgs[0].tool_calls[0].name, "bash") == 0);
        /* tool-ergebnis: rolle + tool_call_id */
        CHECK(msgs != NULL && msgs[1].role == OAI_ROLE_TOOL);
        CHECK(msgs != NULL && strcmp(msgs[1].content, "ergebnis") == 0);
        CHECK(msgs != NULL && msgs[1].tool_call_id != NULL &&
              strcmp(msgs[1].tool_call_id, "call_1") == 0);
        free(msgs);
        msgs = NULL;
    }

    /* system-prompt wird VORANGESTELLT (request-kontext, nicht im
     * transcript: hier nur der sende-seite sichtbar). das transcript
     * enthaelt eine eigene SYSTEM-nachricht -> beide im request */
    chat_clear(&chat);
    CHECK(chat_append(&chat, CHAT_ROLE_SYSTEM, "du bist max agent.") == 0);
    CHECK(chat_append(&chat, CHAT_ROLE_USER, "frage 1") == 0);
    n = send_build_messages(&chat, "angehangener request-kontext", &msgs);
    CHECK(n == 3);
    CHECK(msgs != NULL && msgs[0].role == OAI_ROLE_SYSTEM);
    CHECK(msgs != NULL &&
          strcmp(msgs[0].content, "angehangener request-kontext") == 0);
    CHECK(msgs != NULL && msgs[1].role == OAI_ROLE_SYSTEM);
    CHECK(msgs != NULL && strcmp(msgs[1].content, "du bist max agent.") == 0);
    CHECK(msgs != NULL && msgs[2].role == OAI_ROLE_USER);
    CHECK(msgs != NULL && strcmp(msgs[2].content, "frage 1") == 0);
    free(msgs);
    msgs = NULL;

    /* --- summary (compaction) als USER-nachricht hinter dem system --- */
    char *wrapped = ctx_summary_wrap("aufgabe: tests schreiben");
    CHECK(wrapped != NULL);
    if (wrapped != NULL) {
        n = send_build_messages_from(&chat, 0, "sys", wrapped, &msgs);
        CHECK(n == 4);
        CHECK(msgs != NULL && msgs[0].role == OAI_ROLE_SYSTEM);
        CHECK(msgs != NULL && strcmp(msgs[0].content, "sys") == 0);
        CHECK(msgs != NULL && msgs[1].role == OAI_ROLE_USER);
        CHECK(msgs != NULL && msgs[1].content == wrapped); /* geborgt */
        CHECK(msgs != NULL &&
              strstr(msgs[1].content, "aufgabe: tests") != NULL);
        CHECK(msgs != NULL && msgs[2].role == OAI_ROLE_SYSTEM);
        CHECK(msgs != NULL && msgs[3].role == OAI_ROLE_USER);
        free(msgs);
        msgs = NULL;

        /* leerer wrap (NULL): wie ohne summary */
        n = send_build_messages_from(&chat, 0, "sys", NULL, &msgs);
        CHECK(n == 3);
        free(msgs);
        msgs = NULL;
        free(wrapped);
    }

    /* --- send_find_model --- */
    Config cfg;
    build_cfg(&cfg);

    const Provider *pr = NULL;
    const Model *m = send_find_model(&cfg, "gpt-test", &pr);
    CHECK(m != NULL);
    CHECK(pr != NULL && pr == &cfg.providers[0]);
    CHECK(pr != NULL && pr->api_key != NULL &&
          strcmp(pr->api_key, "sk-test-123") == 0);

    /* modell im zweiten provider */
    pr = NULL;
    m = send_find_model(&cfg, "local-model", &pr);
    CHECK(m != NULL);
    CHECK(pr == &cfg.providers[1]);

    /* unbekannt/leer/NULL */
    CHECK(send_find_model(&cfg, "gibts-nicht", &pr) == NULL);
    CHECK(send_find_model(&cfg, "", &pr) == NULL);
    CHECK(send_find_model(&cfg, NULL, &pr) == NULL);
    CHECK(send_find_model(NULL, "gpt-test", &pr) == NULL);
    CHECK(send_find_model(&cfg, "gpt-test", NULL) != NULL);

    /* --- send_message: benutzungsfehler ohne netzwerk --- */
    AppState st = {0};
    input_init(&st.input);

    /* keine config: fehler landet im verlauf, nicht im crash */
    Config empty = {0};
    CHECK(send_message(&st, &empty) == -1);
    CHECK(st.chat.len == 1);
    CHECK(st.chat.msgs[0].role == CHAT_ROLE_ERROR);
    CHECK(st.chat.msgs[0].text != NULL);
    CHECK(st.chat.msgs[0].text != NULL &&
          strstr(st.chat.msgs[0].text, "kein modell") != NULL);

    /* modell in provider ohne api-key: eigener fehler */
    chat_clear(&st.chat);
    Config nokey = {0};
    nokey.providers = cfg.providers + 1; /* provider[1]: ohne key */
    nokey.providers_len = 1;
    nokey.active_model = dup_str("local-model");
    CHECK(nokey.active_model != NULL);
    CHECK(send_message(&st, &nokey) == -1);
    CHECK(st.chat.len == 1);
    CHECK(st.chat.msgs[0].role == CHAT_ROLE_ERROR);
    CHECK(st.chat.msgs[0].text != NULL &&
          strstr(st.chat.msgs[0].text, "api-key") != NULL);
    free(nokey.active_model);

    chat_free(&st.chat);
    input_free(&st.input);

    /* config aufraeumen (dup_str-pointer) */
    for (size_t p = 0; p < cfg.providers_len; p++) {
        Provider *pp = &cfg.providers[p];
        free(pp->api_key);
        free(pp->base_url);
        for (size_t i = 0; i < pp->models_len; i++) {
            free(pp->models[i].id);
        }
        free(pp->models);
    }
    free(cfg.providers);

    test_stream();
    test_agent();
    test_agent_broken_call();
    test_context_compaction();
    test_compact_now();
    test_context_compaction_fallback();
    test_context_fits();
    test_stream_abort();
    test_stream_no_abort();
    test_agent_realloc();
    test_hooks_ctx();
    test_queue_after_turn();
    test_tools_parallel();
    test_client_reuse();

    chat_free(&chat);
    return test_report();
}