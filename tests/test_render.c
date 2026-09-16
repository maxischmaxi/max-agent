/* tests fuer den inkrementellen scrollback-renderer (draw.c):
 * fertiger content wird GENAU EINMAL gedruckt, die live-zeile und
 * der dock werden neu gezeichnet, und der frame ist rein relativ
 * (cursor-up um genau die vorherigen dock-zeilen). */
#define _POSIX_C_SOURCE 200809L // NOLINT(bugprone-reserved-identifier)

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "chat.h"
#include "command.h"
#include "config.h"
#include "draw.h"
#include "state.h"
#include "test.h"
#include "utils.h"

#define OUT_MAX (1024 * 1024)

/* ausgabe des renderers in einen tmp-file fangen. `raw` behaelt die
 * ansi-sequenzen (fuer frame-geometrie), `text` ist davon befreit
 * (fuer inhalts-pruefungen – im terminal sind die codes unsichtbar) */
typedef struct {
    FILE *f;
    char *raw;
    char *text;
} Capture;

static void cap_open(Capture *c)
{
    /* mehrere captures pro test: alte buffer vorher freigeben
     * (cap_close behaelt sie fuer die assertions) */
    free(c->raw);
    free(c->text);
    c->raw = NULL;
    c->text = NULL;
    c->f = tmpfile();
    CHECK(c->f != NULL);
    c->raw = malloc(OUT_MAX);
    c->text = malloc(OUT_MAX);
    CHECK(c->raw != NULL && c->text != NULL);
    if (c->raw == NULL || c->text == NULL || c->f == NULL) {
        free(c->raw);
        free(c->text);
        c->raw = NULL;
        c->text = NULL;
        return;
    }
    draw_set_out(c->f);
}

/* \x1b-sequenzen, \r und den cursor-block rauswerfen */
static void cap_strip(const char *raw, char *out_buf)
{
    size_t o = 0;
    for (size_t i = 0; raw[i] != '\0';) {
        if (raw[i] == 0x1b) {
            i++;
            if (raw[i] == '[') {
                i++;
                while (raw[i] != '\0' && raw[i] < 0x40) {
                    i++;
                }
                if (raw[i] != '\0') {
                    i++; /* final byte */
                }
                continue;
            }
            continue;
        }
        if (raw[i] == '\r') {
            i++;
            continue;
        }
        /* der utf-8-cursor-block (E2 96 88) bleibt als zeichen
         * stehen: die eingabezeile besteht ohne praefix NUR aus
         * ihm – so zaehlt sie nicht als leerzeile */
        if ((unsigned char)raw[i] == 0xE2 &&
            (unsigned char)raw[i + 1] == 0x96 &&
            (unsigned char)raw[i + 2] == 0x88) {
            out_buf[o++] = raw[i];
            out_buf[o++] = raw[i + 1];
            out_buf[o++] = raw[i + 2];
            i += 3;
            continue;
        }
        out_buf[o++] = raw[i++];
    }
    out_buf[o] = '\0';
}

static void cap_close(Capture *c)
{
    if (c->f == NULL || c->raw == NULL) {
        return;
    }
    rewind(c->f);
    size_t n = fread(c->raw, 1, OUT_MAX - 1, c->f);
    c->raw[n] = '\0';
    fclose(c->f);
    c->f = NULL;
    draw_set_out(NULL);
    cap_strip(c->raw, c->text);
}

static void cap_free(Capture *c)
{
    free(c->raw);
    free(c->text);
    c->raw = NULL;
    c->text = NULL;
}

/* anzahl der Zeilen, die nur aus leerzeichen bestehen (trenner) */
static int count_blank_lines(const char *text)
{
    int n = 0;
    const char *p = text;
    while (*p != '\0') {
        const char *eol = strchr(p, '\n');
        size_t len = (eol != NULL) ? (size_t)(eol - p) : strlen(p);
        if (len > 0 && strspn(p, " ") == len) {
            n++;
        }
        if (eol == NULL) {
            break;
        }
        p = eol + 1;
    }
    return n;
}

static size_t count_str(const char *hay, const char *needle)
{
    size_t n = 0;
    const char *p = hay;
    while ((p = strstr(p, needle)) != NULL) {
        n++;
        p++;
    }
    return n;
}

static void state_setup(AppState *st)
{
    memset(st, 0, sizeof *st);
    input_init(&st->input);
    /* der renderer haelt modul-statischen zustand (frontier,
     * cursor-anker) – pro test frisch, sonst erben die tests
     * einander und lesen in freigegebene chats */
    draw_content_reset();
    FILE *devnull = fopen("/dev/null", "w");
    if (devnull != NULL) {
        draw_set_out(devnull);
        draw_reset(24);
        fclose(devnull);
        draw_set_out(NULL);
    }
}

/* ------------------------------------------------------------------ */
/* content wird genau einmal gedruckt und nie wieder                   */
/* ------------------------------------------------------------------ */
static void test_print_once(void)
{
    AppState st;
    state_setup(&st);
    Config cfg = {0};
    Capture c;

    /* erster frame: leerer chat, nur der dock */
    cap_open(&c);
    draw(24, 80, &st, &cfg);
    cap_close(&c);
    CHECK(strstr(c.text, "model") != NULL); /* statuszeilen */
    CHECK(strstr(c.text, "tokens") != NULL);
    CHECK(strstr(c.text, "\xE2\x96\x88") != NULL); /* eingabefeld */

    /* user-nachricht: erscheint genau EINMAL */
    CHECK(chat_append(&st.chat, CHAT_ROLE_USER, "hallo welt") == 0);
    cap_open(&c);
    draw(24, 80, &st, &cfg);
    cap_close(&c);
    CHECK(strstr(c.text, "you  hallo welt") != NULL);

    /* weitere frames drucken sie nie wieder */
    cap_open(&c);
    draw(24, 80, &st, &cfg);
    draw(24, 80, &st, &cfg);
    cap_close(&c);
    CHECK(count_str(c.text, "hallo welt") == 0);

    cap_free(&c);
    input_free(&st.input);
    chat_free(&st.chat);
}

/* ------------------------------------------------------------------ */
/* mehrzeilige eigene nachricht: label-zeile + eingerueckte folge     */
/* ------------------------------------------------------------------ */
static void test_multiline_user(void)
{
    AppState st;
    state_setup(&st);
    Config cfg = {0};
    Capture c;

    CHECK(chat_append(&st.chat, CHAT_ROLE_USER, "erste zeile\nzweite zeile") ==
          0);
    cap_open(&c);
    draw(24, 80, &st, &cfg);
    cap_close(&c);
    CHECK(count_str(c.text, "you  erste zeile") == 1);
    CHECK(count_str(c.text, "     zweite zeile") == 1); /* 5er-einrueckung */

    cap_free(&c);
    input_free(&st.input);
    chat_free(&st.chat);
}

/* ------------------------------------------------------------------ */
/* assistant antwortet: "ai"-label, text auf derselben spalte wie      */
/* "you"-nachrichten                                                    */
/* ------------------------------------------------------------------ */
static void test_assistant_label(void)
{
    AppState st;
    state_setup(&st);
    Config cfg = {0};
    Capture c;

    CHECK(chat_append(&st.chat, CHAT_ROLE_USER, "frage") == 0);
    CHECK(chat_append(&st.chat, CHAT_ROLE_ASSISTANT, "erste antwort zeile") ==
          0);

    cap_open(&c);
    draw(24, 80, &st, &cfg);
    cap_close(&c);
    CHECK(strstr(c.text, "you  frage") != NULL);
    /* "ai" in derselben breite wie "you": text startet spalte 5 */
    CHECK(strstr(c.text, "ai   erste antwort zeile") != NULL);

    /* umbruch-folgezeile der antwort: auf spalte 5 eingerueckt */
    chat_clear(&st.chat);
    draw_content_reset(); /* wie /new: die frontier mitnehmen */
    CHECK(chat_append(&st.chat, CHAT_ROLE_ASSISTANT,
                      "sehr lange antwort die ganz sicher ueber die breite "
                      "einer zeile hinaus laeuft und darum umgebrochen wird") ==
          0);
    cap_open(&c);
    draw(24, 80, &st, &cfg);
    cap_close(&c);
    /* die fortsetzungszeile beginnt mit 5 leerzeichen */
    CHECK(strstr(c.text, "\n     ") != NULL);

    cap_free(&c);
    input_free(&st.input);
    chat_free(&st.chat);
}

/* ------------------------------------------------------------------ */
/* streaming: die letzte zeile bleibt live, committete zeilen drucken */
/* nur einmal                                                         */
/* ------------------------------------------------------------------ */
static void test_streaming(void)
{
    AppState st;
    state_setup(&st);
    Config cfg = {0};
    Capture c;

    CHECK(chat_append(&st.chat, CHAT_ROLE_USER, "frage") == 0);
    CHECK(chat_append(&st.chat, CHAT_ROLE_ASSISTANT, "") == 0); /* platzh. */
    st.busy = true;

    /* denkphase: die obere rahmenzeile des eingabefelds traegt den
     * spinner + sekunden (im test ohne busy_start: frame 0, "0s");
     * eine live-zeile gibt es in dieser phase NICHT */
    cap_open(&c);
    draw(24, 80, &st, &cfg);
    cap_close(&c);
    CHECK(count_str(c.text, "you  frage") == 1);
    CHECK(strstr(c.text, "0s") != NULL); /* sekunden-zaehler im rahmen */
    CHECK(strstr(c.raw, "\xE2\xA0\x8B") != NULL); /* spinner-frame 0 */
    CHECK(strstr(c.text, "thinking...") == NULL); /* nicht mehr im chat */

    /* erster stream-text: bleibt live, verdraengt das thinking */
    CHECK(chat_append_text(&st.chat, "erster satz") == true);
    cap_open(&c);
    draw(24, 80, &st, &cfg);
    cap_close(&c);
    CHECK(count_str(c.text, "erster satz") == 1);
    CHECK(count_str(c.text, "thinking...") == 0);

    /* text waechst ueber den umbruch: die erste zeile wird
     * committet (einmal gedruckt), die zweite bleibt live */
    CHECK(chat_append_text(&st.chat, " und noch viel mehr text, "
                                     "der ganz sicher ueber die breite "
                                     "hinauslaeuft") == true);
    cap_open(&c);
    draw(24, 80, &st, &cfg);
    draw(24, 80, &st, &cfg);
    cap_close(&c);
    CHECK(count_str(c.text, "erster satz") == 1); /* commit genau einmal */

    /* stream fertig: busy aus, die letzte zeile wird committet */
    st.busy = false;
    cap_open(&c);
    draw(24, 80, &st, &cfg);
    cap_close(&c);
    CHECK(strstr(c.text, "hinauslaeuft") != NULL);

    /* alles final: weitere frames drucken keinen content mehr */
    cap_open(&c);
    draw(24, 80, &st, &cfg);
    cap_close(&c);
    CHECK(count_str(c.text, "erster satz") == 0);
    CHECK(count_str(c.text, "hinauslaeuft") == 0);

    cap_free(&c);
    input_free(&st.input);
    chat_free(&st.chat);
}

/* ------------------------------------------------------------------ */
/* tool-call-zeilen und tool-ergebnisse                              */
/* ------------------------------------------------------------------ */
static void test_tool_rows(void)
{
    AppState st;
    state_setup(&st);
    Config cfg = {0};
    Capture c;

    CHECK(chat_append(&st.chat, CHAT_ROLE_USER, "mach was") == 0);
    CHECK(chat_append(&st.chat, CHAT_ROLE_ASSISTANT, "") == 0);
    ChatToolCall *call = calloc(1, sizeof *call);
    CHECK(call != NULL);
    if (call == NULL) {
        return;
    }
    call->id = dup_str("call_1");
    call->name = dup_str("bash");
    call->arguments = dup_str("{\"command\":\"echo hi\"}");
    CHECK(chat_set_tool_calls(&st.chat, call, 1) == 0);
    CHECK(chat_append_tool(&st.chat, "call_1", "hi\n\n[exit: 0]") == 0);
    CHECK(chat_append(&st.chat, CHAT_ROLE_ASSISTANT, "fertig") == 0);

    cap_open(&c);
    draw(24, 80, &st, &cfg);
    cap_close(&c);
    /* "ai"-label und call in EINER zeile, der pfeil direkt dahinter
     * – KEINE eigene ai-zeile fuer den leeren text */
    CHECK(strstr(c.text, "ai   \xE2\x86\x92 bash({") != NULL);
    /* "ai" genau zweimal: am call und an der antwort "fertig" */
    CHECK(count_str(c.text, "ai   ") == 2);
    /* ergebnis: "output:" vor dem inhalt, exit-code am ende */
    CHECK(strstr(c.text, "output:") != NULL);
    CHECK(strstr(c.text, "hi") != NULL);
    CHECK(strstr(c.raw, "\x1b[32m[exit: 0]") != NULL); /* gruen bei 0 */

    /* exit-code != 0: rot */
    chat_clear(&st.chat);
    draw_content_reset();
    CHECK(chat_append(&st.chat, CHAT_ROLE_USER, "mach") == 0);
    CHECK(chat_append(&st.chat, CHAT_ROLE_ASSISTANT, "") == 0);
    ChatToolCall *call2 = calloc(1, sizeof *call2);
    CHECK(call2 != NULL);
    if (call2 == NULL) {
        return;
    }
    call2->id = dup_str("c2");
    call2->name = dup_str("bash");
    call2->arguments = dup_str("{}");
    CHECK(chat_set_tool_calls(&st.chat, call2, 1) == 0);
    CHECK(chat_append_tool(&st.chat, "c2", "boese\n[exit: 3]") == 0);
    cap_open(&c);
    draw(24, 80, &st, &cfg);
    cap_close(&c);
    CHECK(strstr(c.raw, "\x1b[31m[exit: 3]") != NULL); /* rot bei != 0 */

    cap_free(&c);
    input_free(&st.input);
    chat_free(&st.chat);
}

/* ------------------------------------------------------------------ */
/* frame-geometrie: der zweite frame raeumt genau die dock-zeilen des */
/* ersten und zieht den cursor um genau diese zahl hoch              */
/* ------------------------------------------------------------------ */
static void test_frame_relative(void)
{
    AppState st;
    state_setup(&st);
    Config cfg = {0};
    Capture c;

    /* frame 1: dock = abstand + 2 rahmen + 1 eingabe + 2 status
     * = 6 zeilen */
    cap_open(&c);
    draw(24, 80, &st, &cfg);
    cap_close(&c);
    CHECK(count_str(c.raw, "\x1b[A") == 0); /* erster frame: nichts raeumen */

    /* frame 2: der cursor sitzt auf der LETZTEN der 6 zeilen, hoch
     * gezogen wird darum 5 – und nach dem erase wieder 5 */
    cap_open(&c);
    draw(24, 80, &st, &cfg);
    cap_close(&c);
    CHECK(count_str(c.raw, "\x1b[5A") == 2);

    /* eingabe waechst um eine zeile: dock 7 -> der UEBERNAECHSTE
     * frame raeumt mit up=6 (dieser raeumt noch die alten 6) */
    input_newline(&st.input, 24, 0, false);
    cap_open(&c);
    draw(24, 80, &st, &cfg);
    cap_close(&c);
    CHECK(count_str(c.raw, "\x1b[5A") == 2);
    cap_open(&c);
    draw(24, 80, &st, &cfg);
    cap_close(&c);
    CHECK(count_str(c.raw, "\x1b[6A") == 2);

    /* busy ohne stream-text: KEINE live-zeile (das "thinking"
     * lebt in der rahmenzeile) -> dock bleibt 7 */
    st.busy = true;
    cap_open(&c);
    draw(24, 80, &st, &cfg);
    cap_close(&c);
    cap_open(&c);
    draw(24, 80, &st, &cfg);
    cap_close(&c);
    CHECK(count_str(c.raw, "\x1b[6A") == 2); /* 6 hoch + 6 zurueck */

    /* busy MIT stream-text: die wachsende zeile ist live dazu */
    CHECK(chat_append(&st.chat, CHAT_ROLE_ASSISTANT, "wird getippt") == 0);
    st.busy_start_ms = 0;
    cap_open(&c);
    draw(24, 80, &st, &cfg);
    cap_close(&c);
    cap_open(&c);
    draw(24, 80, &st, &cfg);
    cap_close(&c);
    CHECK(count_str(c.raw, "\x1b[7A") == 2); /* 6 dock + 1 live */

    cap_free(&c);
    input_free(&st.input);
    chat_free(&st.chat);
}

/* ------------------------------------------------------------------ */
/* content-reset (new/resume): nur der schwanz wird gedruckt          */
/* ------------------------------------------------------------------ */
static void test_content_reset(void)
{
    AppState st;
    state_setup(&st);
    Config cfg = {0};
    Capture c;

    /* 40 kurze nachrichten */
    for (int i = 1; i <= 40; i++) {
        char buf[32];
        snprintf(buf, sizeof buf, "zeile %d", i);
        CHECK(chat_append(&st.chat, CHAT_ROLE_USER, buf) == 0);
    }

    /* reset (resume-fall): nur der schwanz drucken */
    draw_content_reset();
    cap_open(&c);
    draw(24, 80, &st, &cfg);
    cap_close(&c);
    CHECK(strstr(c.text, "you  zeile 40") != NULL); /* der schwanz */
    CHECK(strstr(c.text, "you  zeile 2 ") == NULL); /* aelteste fehlen */
    CHECK(strstr(c.text, "you  zeile 10 ") == NULL);

    /* danach normal weiter: neue nachricht einmal drucken */
    CHECK(chat_append(&st.chat, CHAT_ROLE_USER, "neu danach") == 0);
    cap_open(&c);
    draw(24, 80, &st, &cfg);
    cap_close(&c);
    CHECK(count_str(c.text, "neu danach") == 1);

    cap_free(&c);
    input_free(&st.input);
    chat_free(&st.chat);
}

/* ------------------------------------------------------------------ */
/* befehlsliste und dialog im dock                                    */
/* ------------------------------------------------------------------ */
static void test_dock_lists(void)
{
    AppState st;
    state_setup(&st);
    Config cfg = {0};
    Capture c;

    /* "/" tippen: die befehlsliste erscheint im dock */
    input_char(&st.input, '/');
    st.cmd_active = input_in_cmd(&st.input);
    cap_open(&c);
    draw(24, 80, &st, &cfg);
    cap_close(&c);
    CHECK(strstr(c.text, "/clear") != NULL);
    CHECK(strstr(c.text, "/resume") != NULL);
    CHECK(strstr(c.text, "/rename") != NULL);
    input_reset(&st.input);
    st.cmd_active = false;

    /* models-dialog: suchzeile + eintraege statt eingabefeld */
    cfg.providers = calloc(1, sizeof *cfg.providers);
    CHECK(cfg.providers != NULL);
    if (cfg.providers == NULL) {
        return;
    }
    cfg.providers_len = 1;
    Provider *pr = &cfg.providers[0];
    pr->api = OPENAI_COMPLETIONS;
    pr->base_url =
        dup_str("https://example.test"); /* free_config gibt ihn frei */
    pr->models_len = 1;
    pr->models = calloc(1, sizeof *pr->models);
    CHECK(pr->models != NULL);
    if (pr->models == NULL) {
        return;
    }
    pr->models[0].id = dup_str("test-modell"); /* free_config gibt ihn frei */

    st.models_dialog = true;
    cap_open(&c);
    draw(24, 80, &st, &cfg);
    cap_close(&c);
    CHECK(strstr(c.text, "models: ") != NULL);       /* suchzeile */
    CHECK(strstr(c.text, "test-modell") != NULL);    /* eintrag */
    CHECK(strstr(c.text, "keins gewaehlt") != NULL); /* status bleibt */
    st.models_dialog = false;

    cap_free(&c);
    input_free(&st.input);
    chat_free(&st.chat);
    free_config(&cfg);
}

/* leerzeile zwischen den nachrichten: genau eine all-leerzeile
 * zwischen zwei nachrichten, keine vor der ersten */
static void test_separator(void)
{
    AppState st;
    state_setup(&st);
    Config cfg = {0};
    Capture c;

    CHECK(chat_append(&st.chat, CHAT_ROLE_USER, "erste nachricht") == 0);
    cap_open(&c);
    draw(24, 80, &st, &cfg);
    cap_close(&c);
    CHECK(count_str(c.text, "you  erste nachricht") == 1);
    /* erste nachricht: KEIN trenner – nur die abstands-zeile des
     * docks ueber dem eingabefeld zaehlt als leerzeile */
    CHECK(count_blank_lines(c.text) == 1);

    CHECK(chat_append(&st.chat, CHAT_ROLE_ASSISTANT, "die antwort") == 0);
    cap_open(&c);
    draw(24, 80, &st, &cfg);
    cap_close(&c);
    CHECK(count_str(c.text, "die antwort") == 1);
    /* trenner vor der antwort + dock-abstand */
    CHECK(count_blank_lines(c.text) == 2);

    /* dritte nachricht: genau eine leerzeile vor ihr */
    CHECK(chat_append(&st.chat, CHAT_ROLE_USER, "dritte") == 0);
    cap_open(&c);
    draw(24, 80, &st, &cfg);
    cap_close(&c);
    CHECK(count_str(c.text, "you  dritte") == 1);
    /* trenner vor "dritte" + dock-abstand */
    CHECK(count_blank_lines(c.text) == 2);

    cap_free(&c);
    input_free(&st.input);
    chat_free(&st.chat);
}

/* uebergrosser tool-call: bash mit sehr langen parametern geht
 * ueber mehrere zeilen, fortsetzungen eingerueckt, resize-safe
 * (die breite kommt je frame aus cols) */
static void test_tool_call_multiline(void)
{
    AppState st;
    state_setup(&st);
    Config cfg = {0};
    Capture c;

    CHECK(chat_append(&st.chat, CHAT_ROLE_USER, "mach") == 0);
    CHECK(chat_append(&st.chat, CHAT_ROLE_ASSISTANT, "") == 0);
    ChatToolCall *call = calloc(1, sizeof *call);
    CHECK(call != NULL);
    if (call == NULL) {
        return;
    }
    call->id = dup_str("c1");
    call->name = dup_str("bash");
    char args[1024];
    args[0] = '\0';
    strcat(args, "{\"command\":\"");
    for (int i = 0; i < 60; i++) {
        strcat(args, "w00 w01 w02 ");
    }
    strcat(args, "ende\"}");
    call->arguments = dup_str(args);
    CHECK(chat_set_tool_calls(&st.chat, call, 1) == 0);

    cap_open(&c);
    draw(24, 80, &st, &cfg);
    cap_close(&c);
    CHECK(count_str(c.text, "bash(") == 1);       /* pfeil+name 1x */
    CHECK(strstr(c.text, "w00 w01 w02") != NULL); /* argumente da */
    /* fortsetzungs-zeilen: mit TOOL_INDENT_W eingerueckt */
    char *p = c.text;
    int indent_lines = 0;
    while ((p = strstr(p, "\n  ")) != NULL) {
        indent_lines++;
        p += 3;
    }
    CHECK(indent_lines >= 1);
    /* der call geht ueber mehr als eine zeile */
    CHECK(strstr(c.text, "bash(") != strstr(c.text, "ende\"}"));

    cap_free(&c);
    input_free(&st.input);
    chat_free(&st.chat);
}

/* tool-spinner: laeuft die ki an einem tool, zeigt der chat (direkt
 * unter dem call) einen live-spinner; der leichte tick aktualisiert
 * nur ihn und die rahmen-zeile */
static void test_tool_spinner(void)
{
    AppState st;
    state_setup(&st);
    Config cfg = {0};
    Capture c;

    CHECK(chat_append(&st.chat, CHAT_ROLE_USER, "mach") == 0);
    CHECK(chat_append(&st.chat, CHAT_ROLE_ASSISTANT, "") == 0);
    ChatToolCall *call = calloc(1, sizeof *call);
    CHECK(call != NULL);
    if (call == NULL) {
        return;
    }
    call->id = dup_str("c");
    call->name = dup_str("bash");
    call->arguments = dup_str("{}");
    CHECK(chat_set_tool_calls(&st.chat, call, 1) == 0);

    st.busy = true;
    st.tool_running = true;
    st.busy_start_ms = 0; /* 0s: deterministischer spinner-frame */
    cap_open(&c);
    draw(24, 80, &st, &cfg);
    cap_close(&c);
    /* der call ist committet (mit ai-label), darunter der live-
     * spinner (5er-einrueckung; der rahmen-spinner der input-zeile
     * sieht aehnlich aus, deshalb pruefen wir die einrueckung) */
    CHECK(strstr(c.text, "ai   \xE2\x86\x92 bash({}") != NULL);
    CHECK(strstr(c.text, "     \xE2\xA0\x8B") != NULL); /* spinner */

    /* der leichte tick: KEIN voller frame (kein cursor-up um die
     * ganze dock-hoehe + kein erase des docks), nur die spinner-
     * zeilen an ort und stelle */
    cap_open(&c);
    draw_busy_tick(24, 80, &st, &cfg);
    cap_close(&c);
    CHECK(strstr(c.text, "     \xE2\xA0\x8B") != NULL); /* spinner dabei */
    /* KEIN voller erase (up + alle zeilen loeschen + up, zwei
     * positioning-ups): nur EIN up auf die live-zeile und genau
     * zwei zeilen-rewrites (spinner + rahmen) */
    CHECK(count_str(c.raw, "\x1b[6A") == 1);
    CHECK(count_str(c.raw, "\x1b[K") == 2);

    /* tool fertig: live-spinner weg (der rahmen-spinner des docks
     * bleibt – busy ist noch an) */
    st.tool_running = false;
    cap_open(&c);
    draw(24, 80, &st, &cfg);
    cap_close(&c);
    CHECK(strstr(c.text, "     \xE2\xA0\x8B") == NULL);

    cap_free(&c);
    input_free(&st.input);
    chat_free(&st.chat);
}

/* ctrl+c-warnung: solange confirm_quit ansteht, zeigt die abstands-
 * zeile ueber dem eingabefeld die warnung; jede andere taste hebt
 * sie auf und die zeile ist wieder leer */
static void test_quit_warning(void)
{
    AppState st;
    state_setup(&st);
    Config cfg = {0};
    Capture c;

    st.confirm_quit = true;
    cap_open(&c);
    draw(24, 80, &st, &cfg);
    cap_close(&c);
    CHECK(strstr(c.text, "quit? ctrl+c again to confirm") != NULL);

    /* dock-hoehe aendert sich dabei NICHT: die zeile ist dieselbe,
     * nur ihr inhalt wechselt (frame-geometrie bleibt stabil) */
    st.confirm_quit = false;
    cap_open(&c);
    draw(24, 80, &st, &cfg);
    cap_close(&c);
    CHECK(strstr(c.text, "quit?") == NULL);
    /* leerzeile (dock-abstand) ist weiter da */
    CHECK(count_blank_lines(c.text) == 1);

    cap_free(&c);
    input_free(&st.input);
    chat_free(&st.chat);
}

int main(void)
{
    test_print_once();
    test_multiline_user();
    test_assistant_label();
    test_streaming();
    test_tool_rows();
    test_frame_relative();
    test_content_reset();
    test_dock_lists();
    test_separator();
    test_tool_call_multiline();
    test_tool_spinner();
    test_quit_warning();
    return test_report();
}