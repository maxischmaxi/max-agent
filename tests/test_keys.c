#define _POSIX_C_SOURCE 200809L // NOLINT(bugprone-reserved-identifier)

#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "chat.h"
#include "config.h"
#include "draw.h"
#include "history.h"
#include "keys.h"
#include "settings.h"
#include "state.h"
#include "test.h"
#include "utils.h"

static void test_key_from_byte(void)
{
    CHECK(key_from_byte('a').kind == KEY_CHAR);
    CHECK(key_from_byte('a').ch == 'a');
    CHECK(key_from_byte('\r').kind == KEY_ENTER);
    CHECK(key_from_byte('\n').kind == KEY_NEWLINE);
    CHECK(key_from_byte(0x03).kind == KEY_CTRL_C);
    CHECK(key_from_byte(0x11).kind == KEY_CTRL_Q);
    CHECK(key_from_byte(0x7f).kind == KEY_BACKSPACE);
    CHECK(key_from_byte('\t').kind == KEY_TAB); /* tab = byte 0x09 */
    /* POSIX/readline-shortcuts: raw ctrl-bytes */
    CHECK(key_from_byte(0x01).kind == KEY_CTRL_A);
    CHECK(key_from_byte(0x05).kind == KEY_CTRL_E);
    CHECK(key_from_byte(0x02).kind == KEY_CTRL_B);
    CHECK(key_from_byte(0x06).kind == KEY_CTRL_F);
    CHECK(key_from_byte(0x17).kind == KEY_CTRL_W);
    CHECK(key_from_byte(0x15).kind == KEY_CTRL_U);
    CHECK(key_from_byte(0x0b).kind == KEY_CTRL_K);
    CHECK(key_from_byte(0x04).kind == KEY_CTRL_D);
    CHECK(key_from_byte(0x08).kind == KEY_CTRL_H);
    CHECK(key_from_byte(0x0c).kind == KEY_CTRL_L);
    CHECK(key_from_byte(0x10).kind == KEY_CTRL_P);
    CHECK(key_from_byte(0x0e).kind == KEY_CTRL_N);
    CHECK(key_from_byte(0x1a).kind == KEY_NONE); /* ctrl+z: nicht belegt */
    CHECK(key_from_byte(0x14).kind == KEY_CTRL_T);
    CHECK(key_from_byte(0x00).kind == KEY_NONE); /* unbenannte bytes */
}

static void test_key_from_escape(void)
{
    /* lone esc */
    CHECK(key_from_escape("\x1b", 1).kind == KEY_ESCAPE);
    /* esc + CR = alt+enter = newline */
    CHECK(key_from_escape("\x1b\r", 2).kind == KEY_NEWLINE);
    /* alt+ctrl+c / alt+ctrl+q (kitty: CSI codepoint;mods u) */
    CHECK(key_from_escape("\x1b[99;5u", 7).kind == KEY_CTRL_C);
    CHECK(key_from_escape("\x1b[113;5u", 8).kind == KEY_CTRL_Q);
    /* shift+enter (CSI 13;2u) */
    CHECK(key_from_escape("\x1b[13;2u", 7).kind == KEY_NEWLINE);
    /* pfeiltasten (CSI A/B) */
    CHECK(key_from_escape("\x1b[A", 3).kind == KEY_UP);
    CHECK(key_from_escape("\x1b[B", 3).kind == KEY_DOWN);
    /* page up/down (CSI 5~/6~): chat-verlauf blaettern */
    CHECK(key_from_escape("\x1b[5~", 4).kind == KEY_PGUP);
    CHECK(key_from_escape("\x1b[6~", 4).kind == KEY_PGDN);
    CHECK(key_from_escape("\x1b[5;5~", 6).kind == KEY_NONE); /* mit modi */
    /* kitty-protokoll: tab als CSI 9u */
    CHECK(key_from_escape("\x1b[9u", 4).kind == KEY_TAB);
    /* kitty: ctrl+X als CSI <codepoint>;5u */
    CHECK(key_from_escape("\x1b[97;5u", 7).kind == KEY_CTRL_A);
    CHECK(key_from_escape("\x1b[119;5u", 8).kind == KEY_CTRL_W);
    CHECK(key_from_escape("\x1b[117;5u", 8).kind == KEY_CTRL_U);
    CHECK(key_from_escape("\x1b[104;5u", 8).kind == KEY_CTRL_H);
    /* kitty: ohne ctrl-modifikator kein shortcut */
    CHECK(key_from_escape("\x1b[119;2u", 8).kind == KEY_NONE); /* shift+w */
    CHECK(key_from_escape("\x1b[119u", 6).kind == KEY_NONE);   /* plain w  */
    /* meta-bindings: legacy-encoding (ESC + zeichen).
     * wichtig: "\x1b" "b" getrennt schreiben – "\x1bb" waere
     * EINE hex-escape (b ist eine hex-ziffer) */
    CHECK(key_from_escape("\x1b"
                          "b",
                          2)
              .kind == KEY_ALT_B);
    CHECK(key_from_escape("\x1b"
                          "f",
                          2)
              .kind == KEY_ALT_F);
    CHECK(key_from_escape("\x1b\x7f", 2).kind == KEY_ALT_BACKSPACE);
    CHECK(key_from_escape("\x1b"
                          "B",
                          2)
              .kind == KEY_ALT_B); /* alt+shift */
    /* meta-bindings: kitty-encoding (CSI <cp>;3u, 3 = alt) */
    CHECK(key_from_escape("\x1b[98;3u", 7).kind == KEY_ALT_B);
    CHECK(key_from_escape("\x1b[127;3u", 9).kind == KEY_ALT_BACKSPACE);
    /* esc ohne meta-byte bleibt escape, unbekannte meta-keys none */
    CHECK(key_from_escape("\x1b", 1).kind == KEY_ESCAPE);
    CHECK(key_from_escape("\x1b"
                          "z",
                          2)
              .kind == KEY_NONE); /* alt+z: frei */
    /* shift+tab (CSI Z): noch ohne eigene meaning */
    CHECK(key_from_escape("\x1b[Z", 3).kind == KEY_NONE);
    /* lock-modifikatoren (num lock 128, caps lock 64) stecken mit im
     * mods-feld und duerfen den shortcut nicht kippen */
    CHECK(key_from_escape("\x1b[98;131u", 9).kind == KEY_ALT_B);
    CHECK(key_from_escape("\x1b[119;69u", 9).kind == KEY_CTRL_W);
    /* alt+shift (mods 4 = shift|alt) */
    CHECK(key_from_escape("\x1b[98;4u", 7).kind == KEY_ALT_B);
    /* xterm modifyOtherKeys: CSI 27;<mods>;<codepoint>~ */
    CHECK(key_from_escape("\x1b[27;3;98~", 10).kind == KEY_ALT_B);
    CHECK(key_from_escape("\x1b[27;5;119~", 11).kind == KEY_CTRL_W);
    CHECK(key_from_escape("\x1b[27;2;13~", 10).kind == KEY_NEWLINE);
    /* kitty meldet auch esc und enter als CSI u */
    CHECK(key_from_escape("\x1b[27u", 5).kind == KEY_ESCAPE);
    CHECK(key_from_escape("\x1b[13u", 5).kind == KEY_ENTER);
}

static void test_key_seq_len(void)
{
    /* eine taste = eine sequenz, der rest gehoert zur naechsten */
    CHECK(key_seq_len("abc", 3) == 1);
    CHECK(key_seq_len("\x1b[98;3ux", 8) == 7);
    CHECK(key_seq_len("\x1b[98;3u\x1b[102;3u", 15) == 7);
    CHECK(key_seq_len("\x1b"
                      "bx",
                      3) == 2);
    CHECK(key_seq_len("\x1b[A", 3) == 3);
    CHECK(key_seq_len("\x1bOA", 3) == 3);
    /* esc esc: das erste esc ist fuer sich fertig */
    CHECK(key_seq_len("\x1b\x1b[A", 4) == 1);
    /* unvollstaendig: 0 = auf mehr bytes warten */
    CHECK(key_seq_len("", 0) == 0);
    CHECK(key_seq_len("\x1b", 1) == 0);
    CHECK(key_seq_len("\x1b[", 2) == 0);
    CHECK(key_seq_len("\x1b[98;3", 6) == 0);
}

static void test_key_read_batch(void)
{
    /* mehrere tasten aus EINEM read: keine darf verloren gehen
     * (tastenwiederholung, schnelles tippen nach einem shortcut) */
    keys_unread("\x1b[98;3ux", 8);
    CHECK(key_read().kind == KEY_ALT_B);
    Key c = key_read();
    CHECK(c.kind == KEY_CHAR && c.ch == 'x');

    keys_unread("\x1b[98;3u\x1b[102;3u", 15);
    CHECK(key_read().kind == KEY_ALT_B);
    CHECK(key_read().kind == KEY_ALT_F);

    keys_unread("\x1b"
                "b\x17z",
                4);
    CHECK(key_read().kind == KEY_ALT_B);
    CHECK(key_read().kind == KEY_CTRL_W);
    CHECK(key_read().ch == 'z');
}

/* keys_abort_pressed: der poll waehrend einer laufenden anfrage.
 * darf nie blockieren, muss ctrl+c/esc melden und alles andere
 * unberuehrt lassen. */
static void test_abort_poll(void)
{
    /* --- quelle 1: der tasten-puffer --- */
    keys_unread("abc", 3);
    CHECK(!keys_abort_pressed()); /* nichts zum abbrechen */
    /* die zeichen sind noch da */
    CHECK(key_read().ch == 'a');
    CHECK(key_read().ch == 'b');
    CHECK(key_read().ch == 'c');

    keys_unread("ab\x03"
                "cd",
                5);
    CHECK(keys_abort_pressed());
    /* nach dem abbruch ist der puffer leer: wer stoppt, tippt nicht */
    keys_unread("z", 1);
    CHECK(key_read().ch == 'z');

    keys_unread("\x1b", 1); /* escape allein zaehlt auch */
    CHECK(keys_abort_pressed());

    /* ABER: pfeiltasten & co. fangen ebenfalls mit 0x1b an und
     * duerfen die laufende antwort nicht stoppen */
    keys_unread("\x1b[A", 3); /* pfeil hoch */
    CHECK(!keys_abort_pressed());
    CHECK(key_read().kind == KEY_UP); /* unberuehrt im puffer */

    keys_unread("\x1b[B", 3); /* pfeil runter */
    CHECK(!keys_abort_pressed());
    CHECK(key_read().kind == KEY_DOWN);

    keys_unread("\x1bOH", 3); /* SS3: pos1 */
    CHECK(!keys_abort_pressed());
    (void)key_read();

    /* eine sequenz schuetzt ein danach folgendes ctrl+c nicht */
    keys_unread("\x1b[A\x03", 4);
    CHECK(keys_abort_pressed());

    /* zwei escapes hintereinander: das erste steht fuer sich */
    keys_unread("\x1b\x1b", 2);
    CHECK(keys_abort_pressed());

    /* --- quelle 2: stdin, per pipe untergeschoben --- */
    int saved = dup(STDIN_FILENO);
    CHECK(saved >= 0);
    if (saved < 0) {
        return;
    }
    int fds[2];
    CHECK(pipe(fds) == 0);
    CHECK(dup2(fds[0], STDIN_FILENO) >= 0);

    /* leere pipe: kein abbruch, und der aufruf blockiert nicht */
    CHECK(!keys_abort_pressed());

    CHECK(write(fds[1], "xy", 2) == 2);
    CHECK(!keys_abort_pressed());
    /* die bytes sind nicht verloren, sondern zurueck im puffer */
    CHECK(key_read().ch == 'x');
    CHECK(key_read().ch == 'y');

    CHECK(write(fds[1],
                "q\x03"
                "q",
                3) == 3);
    CHECK(keys_abort_pressed());

    /* halbe sequenz: noch nicht entscheiden, der rest kann folgen.
     * die leere pipe macht das auslesen danach deterministisch. */
    keys_unread("\x1b[", 2);
    CHECK(!keys_abort_pressed());
    (void)key_read(); /* puffer leeren fuer die folgetests */

    CHECK(dup2(saved, STDIN_FILENO) >= 0);
    close(saved);
    close(fds[0]);
    close(fds[1]);
}

static void test_keys_unread_fifo(void)
{
    /* key_read greift zuerst auf pending zu - ohne stdin zu lesen */
    keys_unread("ab", 2);
    Key k1 = key_read();
    CHECK(k1.kind == KEY_CHAR && k1.ch == 'a');
    Key k2 = key_read();
    CHECK(k2.kind == KEY_CHAR && k2.ch == 'b');

    /* prepend: neue bytes landen VOR vorhandenen */
    keys_unread("yz", 2);
    CHECK(key_read().ch == 'y');
    keys_unread("x", 1); /* vor 'z' einsortiert */
    CHECK(key_read().ch == 'x');
    CHECK(key_read().ch == 'z');

    /* randfaelle */
    keys_unread(NULL, 4); /* kein crash */
    keys_unread("", 0);
}

/* ------------------------------------------------------------------ */
/* stdin auf eine leere pipe legen. key_read() prueft nach einem      */
/* einzelnen escape, ob noch eine sequenz nachkommt – an einem        */
/* echten terminal, an /dev/null oder an einer pipe mit daten faellt  */
/* das je nach umgebung anders aus. eine leere pipe ist einfach nie   */
/* lesbar, damit wird das verhalten deterministisch.                  */
/* ------------------------------------------------------------------ */
typedef struct {
    int saved;
    int fds[2];
} StdinPipe;

static bool stdin_pipe_open(StdinPipe *p)
{
    p->saved = dup(STDIN_FILENO);
    CHECK(p->saved >= 0);
    if (p->saved < 0) {
        return false;
    }
    CHECK(pipe(p->fds) == 0);
    CHECK(dup2(p->fds[0], STDIN_FILENO) >= 0);
    return true;
}

static void stdin_pipe_close(StdinPipe *p)
{
    CHECK(dup2(p->saved, STDIN_FILENO) >= 0);
    close(p->saved);
    close(p->fds[0]);
    close(p->fds[1]);
}

/* pfeiltasten im chat-feld: in einer mehrzeiligen eingabe wechseln
 * sie erst die zeile, an den raendern gehen sie in die history.
 * gefahren wird ueber handle_key(), also die echte kette. */
static void test_history_keys(void)
{
    /* stdin auf eine leere pipe legen: key_read() prueft nach einem
     * einzelnen escape, ob noch eine sequenz nachkommt. an einem
     * echten terminal oder an /dev/null faellt das je nach umgebung
     * anders aus – die leere pipe ist einfach nie lesbar. */
    StdinPipe sp;
    if (!stdin_pipe_open(&sp)) {
        return;
    }

    AppState st = {0};
    input_init(&st.input);
    Config cfg = {0};

    /* zwei eintraege in die history, ohne senden (das wuerde die
     * api rufen) */
    history_add(&st.history, "alte frage");
    history_add(&st.history, "neue frage");

    /* --- leeres feld: hoch holt den juengsten eintrag --- */
    keys_unread("\x1b[A", 3); /* pfeil hoch */
    handle_key(&st, &cfg, 24, 80);
    CHECK(st.input.count == 1);
    CHECK(strcmp(st.input.lines[0], "neue frage") == 0);

    keys_unread("\x1b[A", 3);
    handle_key(&st, &cfg, 24, 80);
    CHECK(strcmp(st.input.lines[0], "alte frage") == 0);

    /* am aeltesten ende bleibt die eingabe stehen */
    keys_unread("\x1b[A", 3);
    handle_key(&st, &cfg, 24, 80);
    CHECK(strcmp(st.input.lines[0], "alte frage") == 0);

    /* runter fuehrt zurueck bis zum leeren entwurf */
    keys_unread("\x1b[B", 3); /* pfeil runter */
    handle_key(&st, &cfg, 24, 80);
    CHECK(strcmp(st.input.lines[0], "neue frage") == 0);
    keys_unread("\x1b[B", 3);
    handle_key(&st, &cfg, 24, 80);
    CHECK(st.input.lines[0][0] == '\0'); /* entwurf war leer */

    /* --- mehrzeilig: die pfeile wechseln erst die zeile --- */
    input_set_text(&st.input, "zeile1\nzeile2\nzeile3");
    CHECK(st.input.cursor_line == 2);

    keys_unread("\x1b[A", 3);
    handle_key(&st, &cfg, 24, 80);
    CHECK(st.input.cursor_line == 1); /* nur cursor, text bleibt */
    CHECK(st.input.count == 3);

    keys_unread("\x1b[A", 3);
    handle_key(&st, &cfg, 24, 80);
    CHECK(st.input.cursor_line == 0);
    CHECK(st.input.count == 3);

    /* erst OBEN angekommen geht es in die history */
    keys_unread("\x1b[A", 3);
    handle_key(&st, &cfg, 24, 80);
    CHECK(st.input.count == 1);
    CHECK(strcmp(st.input.lines[0], "neue frage") == 0);

    /* der mehrzeilige entwurf kommt vollstaendig zurueck */
    keys_unread("\x1b[B", 3);
    handle_key(&st, &cfg, 24, 80);
    CHECK(st.input.count == 3);
    CHECK(strcmp(st.input.lines[0], "zeile1") == 0);
    CHECK(strcmp(st.input.lines[2], "zeile3") == 0);

    /* --- ctrl+p/ctrl+n gehen immer in die history --- */
    input_set_text(&st.input, "a\nb");
    CHECK(st.input.cursor_line == 1);
    keys_unread("\x10", 1); /* ctrl+p, mitten in der eingabe */
    handle_key(&st, &cfg, 24, 80);
    CHECK(st.input.count == 1);
    CHECK(strcmp(st.input.lines[0], "neue frage") == 0);
    keys_unread("\x0e", 1); /* ctrl+n */
    handle_key(&st, &cfg, 24, 80);
    CHECK(st.input.count == 2);
    CHECK(strcmp(st.input.lines[0], "a") == 0);

    /* --- escape verwirft den entwurf --- */
    keys_unread("\x10", 1); /* ctrl+p: history, egal welche zeile */
    handle_key(&st, &cfg, 24, 80);
    CHECK(strcmp(st.input.lines[0], "neue frage") == 0);
    keys_unread("\x1b", 1); /* escape */
    handle_key(&st, &cfg, 24, 80);
    CHECK(st.input.lines[0][0] == '\0');
    CHECK(st.history.pos == 0);
    CHECK(st.history.draft == NULL);

    input_free(&st.input);
    chat_free(&st.chat);
    history_free(&st.history);
    stdin_pipe_close(&sp);
}

/* settings -> system prompt: das untermenue setzt die drei
 * config-zustaende, "edit" uebergibt an das eingabefeld.
 * gefahren ueber handle_key(), also die echte kette.
 *
 * WICHTIG: die handler rufen config_persist(), das ueber $HOME in
 * die echte config schreiben wuerde. der test biegt HOME auf ein
 * wegwerf-verzeichnis um. */
static void test_prompt_setting(void)
{
    char tmpl[] = "/tmp/max-agent-keys-XXXXXX";
    char *home = mkdtemp(tmpl);
    CHECK(home != NULL);
    if (home == NULL) {
        return;
    }
    StdinPipe sp;
    if (!stdin_pipe_open(&sp)) {
        return;
    }

    char *old_home = getenv("HOME");
    char saved[512] = {0};
    if (old_home != NULL) {
        snprintf(saved, sizeof saved, "%s", old_home);
    }
    CHECK(setenv("HOME", home, 1) == 0);

    AppState st = {0};
    input_init(&st.input);
    Config cfg = {0};

    /* --- settings oeffnen und "system prompt" waehlen --- */
    st.settings_dialog = true;
    st.dialog = (DialogState){0};
    snprintf(st.dialog.search, sizeof st.dialog.search, "system");
    CHECK(ui_mode(&st) == MODE_SETTINGS);
    keys_unread("\r", 1);
    handle_key(&st, &cfg, 24, 80);
    CHECK(st.prompt_sub);
    CHECK(ui_mode(&st) == MODE_PROMPT);

    /* --- "off" setzt den leeren string --- */
    snprintf(st.dialog.search, sizeof st.dialog.search, "off");
    keys_unread("\r", 1);
    handle_key(&st, &cfg, 24, 80);
    CHECK(cfg.system_prompt != NULL && cfg.system_prompt[0] == '\0');
    CHECK(st.prompt_sub); /* dialog bleibt offen */

    /* --- "default" setzt zurueck auf NULL --- */
    snprintf(st.dialog.search, sizeof st.dialog.search, "default");
    keys_unread("\r", 1);
    handle_key(&st, &cfg, 24, 80);
    CHECK(cfg.system_prompt == NULL);

    /* --- "edit" schliesst den dialog und oeffnet das feld --- */
    snprintf(st.dialog.search, sizeof st.dialog.search, "edit");
    keys_unread("\r", 1);
    handle_key(&st, &cfg, 24, 80);
    CHECK(st.prompt_edit);
    CHECK(!st.settings_dialog);
    CHECK(!st.prompt_sub);
    CHECK(ui_mode(&st) == MODE_INPUT);
    CHECK(st.input.lines[0][0] == '\0'); /* default -> leer starten */

    /* tippen und speichern */
    keys_unread("du bist knapp\r", 14);
    handle_key(&st, &cfg, 24, 80); /* 'd' */
    for (int i = 0; i < 13; i++) {
        handle_key(&st, &cfg, 24, 80);
    }
    CHECK(!st.prompt_edit); /* enter hat gespeichert */
    CHECK(cfg.system_prompt != NULL &&
          strcmp(cfg.system_prompt, "du bist knapp") == 0);
    CHECK(st.input.lines[0][0] == '\0'); /* feld wieder frei */
    /* der prompt-text darf NICHT in der nachrichten-history landen */
    CHECK(st.history.len == 0);
    CHECK(st.chat.len == 0); /* und auch nicht im verlauf */

    /* --- erneutes "edit" legt den bestehenden text vor --- */
    st.settings_dialog = true;
    st.prompt_sub = true;
    st.dialog = (DialogState){0};
    snprintf(st.dialog.search, sizeof st.dialog.search, "edit");
    keys_unread("\r", 1);
    handle_key(&st, &cfg, 24, 80);
    CHECK(st.prompt_edit);
    CHECK(strcmp(st.input.lines[0], "du bist knapp") == 0);

    /* --- escape verwirft die bearbeitung --- */
    keys_unread("x", 1);
    handle_key(&st, &cfg, 24, 80);
    keys_unread("\x1b", 1);
    handle_key(&st, &cfg, 24, 80);
    CHECK(!st.prompt_edit);
    CHECK(cfg.system_prompt != NULL &&
          strcmp(cfg.system_prompt, "du bist knapp") == 0); /* unveraendert */

    /* --- leeres feld speichern = zurueck zur vorlage --- */
    st.prompt_edit = true;
    input_set_text(&st.input, "");
    keys_unread("\r", 1);
    handle_key(&st, &cfg, 24, 80);
    CHECK(cfg.system_prompt == NULL);
    CHECK(!st.prompt_edit);

    free_config(&cfg);
    input_free(&st.input);
    chat_free(&st.chat);
    history_free(&st.history);

    if (saved[0] != '\0') {
        CHECK(setenv("HOME", saved, 1) == 0);
    } else {
        CHECK(unsetenv("HOME") == 0);
    }

    stdin_pipe_close(&sp);

    /* das wegwerf-home wieder abraeumen, sonst sammelt sich bei
     * jedem testlauf ein verzeichnis in /tmp an. das layout ist
     * bekannt, also ohne shell-aufruf. */
    char path[600];
    (void)snprintf(path, sizeof path, "%s/.config/.maxagent/config.json", home);
    (void)unlink(path);
    (void)snprintf(path, sizeof path, "%s/.config/.maxagent", home);
    (void)rmdir(path);
    (void)snprintf(path, sizeof path, "%s/.config", home);
    (void)rmdir(path);
    CHECK(rmdir(home) == 0);
}

/* pfeil-hoch in einer UMGEBROCHENEN zeile muss sichtbar eine zeile
 * hoch gehen, nicht die history holen: der benutzer sieht mehrere
 * zeilen, auch wenn es logisch nur eine ist. */
static void test_wrapped_arrows(void)
{
    StdinPipe sp;
    if (!stdin_pipe_open(&sp)) {
        return;
    }

    AppState st = {0};
    input_init(&st.input);
    Config cfg = {0};

    history_add(&st.history, "alter eintrag");

    /* eine logische zeile, die bei 40 spalten dreimal umbricht */
    const int cols = 40;
    int w = input_field_width(cols);
    size_t n = (size_t)w * 3;
    char *lang = malloc(n + 1);
    CHECK(lang != NULL);
    if (lang == NULL) {
        return;
    }
    memset(lang, 'x', n);
    lang[n] = '\0';
    input_set_text(&st.input, lang);
    CHECK(st.input.count == 1); /* logisch EINE zeile */
    CHECK(input_screen_rows(&st.input, w) >= 3);

    size_t row = 0;
    input_cursor_screen(&st.input, w, &row, NULL);
    CHECK(row >= 2); /* cursor steht ganz unten */

    /* zweimal hoch: bleibt im text, history unberuehrt */
    for (int i = 0; i < 2; i++) {
        keys_unread("\x1b[A", 3);
        handle_key(&st, &cfg, 24, cols);
        CHECK(st.input.count == 1);
        CHECK(strcmp(st.input.lines[0], lang) == 0);
    }
    input_cursor_screen(&st.input, w, &row, NULL);
    CHECK(row == 0); /* oben angekommen */

    /* erst JETZT greift die history */
    keys_unread("\x1b[A", 3);
    handle_key(&st, &cfg, 24, cols);
    CHECK(strcmp(st.input.lines[0], "alter eintrag") == 0);

    free(lang);
    input_free(&st.input);
    chat_free(&st.chat);
    history_free(&st.history);
    stdin_pipe_close(&sp);
}

int main(void)
{
    test_key_from_byte();
    test_key_from_escape();
    test_key_seq_len();
    test_keys_unread_fifo();
    test_abort_poll();
    test_history_keys();
    test_prompt_setting();
    test_wrapped_arrows();
    test_key_read_batch();
    return test_report();
}