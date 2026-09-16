#define _POSIX_C_SOURCE 200809L // NOLINT(bugprone-reserved-identifier)

#include <string.h>
#include <unistd.h>

#include "chat.h"
#include "config.h"
#include "history.h"
#include "keys.h"
#include "state.h"
#include "test.h"

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

/* pfeiltasten im chat-feld: in einer mehrzeiligen eingabe wechseln
 * sie erst die zeile, an den raendern gehen sie in die history.
 * gefahren wird ueber handle_key(), also die echte kette. */
static void test_history_keys(void)
{
    /* stdin auf eine leere pipe legen: key_read() prueft nach einem
     * einzelnen escape, ob noch eine sequenz nachkommt. an einem
     * echten terminal oder an /dev/null faellt das je nach umgebung
     * anders aus – die leere pipe ist einfach nie lesbar. */
    int saved = dup(STDIN_FILENO);
    CHECK(saved >= 0);
    if (saved < 0) {
        return;
    }
    int fds[2];
    CHECK(pipe(fds) == 0);
    CHECK(dup2(fds[0], STDIN_FILENO) >= 0);

    AppState st = {0};
    input_init(&st.input);
    Config cfg = {0};
    DebugState dbg = {0};

    /* zwei eintraege in die history, ohne senden (das wuerde die
     * api rufen) */
    history_add(&st.history, "alte frage");
    history_add(&st.history, "neue frage");

    /* --- leeres feld: hoch holt den juengsten eintrag --- */
    keys_unread("\x1b[A", 3); /* pfeil hoch */
    handle_key(&st, &cfg, &dbg, 24, 80);
    CHECK(st.input.count == 1);
    CHECK(strcmp(st.input.lines[0], "neue frage") == 0);

    keys_unread("\x1b[A", 3);
    handle_key(&st, &cfg, &dbg, 24, 80);
    CHECK(strcmp(st.input.lines[0], "alte frage") == 0);

    /* am aeltesten ende bleibt die eingabe stehen */
    keys_unread("\x1b[A", 3);
    handle_key(&st, &cfg, &dbg, 24, 80);
    CHECK(strcmp(st.input.lines[0], "alte frage") == 0);

    /* runter fuehrt zurueck bis zum leeren entwurf */
    keys_unread("\x1b[B", 3); /* pfeil runter */
    handle_key(&st, &cfg, &dbg, 24, 80);
    CHECK(strcmp(st.input.lines[0], "neue frage") == 0);
    keys_unread("\x1b[B", 3);
    handle_key(&st, &cfg, &dbg, 24, 80);
    CHECK(st.input.lines[0][0] == '\0'); /* entwurf war leer */

    /* --- mehrzeilig: die pfeile wechseln erst die zeile --- */
    input_set_text(&st.input, "zeile1\nzeile2\nzeile3");
    CHECK(st.input.cursor_line == 2);

    keys_unread("\x1b[A", 3);
    handle_key(&st, &cfg, &dbg, 24, 80);
    CHECK(st.input.cursor_line == 1); /* nur cursor, text bleibt */
    CHECK(st.input.count == 3);

    keys_unread("\x1b[A", 3);
    handle_key(&st, &cfg, &dbg, 24, 80);
    CHECK(st.input.cursor_line == 0);
    CHECK(st.input.count == 3);

    /* erst OBEN angekommen geht es in die history */
    keys_unread("\x1b[A", 3);
    handle_key(&st, &cfg, &dbg, 24, 80);
    CHECK(st.input.count == 1);
    CHECK(strcmp(st.input.lines[0], "neue frage") == 0);

    /* der mehrzeilige entwurf kommt vollstaendig zurueck */
    keys_unread("\x1b[B", 3);
    handle_key(&st, &cfg, &dbg, 24, 80);
    CHECK(st.input.count == 3);
    CHECK(strcmp(st.input.lines[0], "zeile1") == 0);
    CHECK(strcmp(st.input.lines[2], "zeile3") == 0);

    /* --- ctrl+p/ctrl+n gehen immer in die history --- */
    input_set_text(&st.input, "a\nb");
    CHECK(st.input.cursor_line == 1);
    keys_unread("\x10", 1); /* ctrl+p, mitten in der eingabe */
    handle_key(&st, &cfg, &dbg, 24, 80);
    CHECK(st.input.count == 1);
    CHECK(strcmp(st.input.lines[0], "neue frage") == 0);
    keys_unread("\x0e", 1); /* ctrl+n */
    handle_key(&st, &cfg, &dbg, 24, 80);
    CHECK(st.input.count == 2);
    CHECK(strcmp(st.input.lines[0], "a") == 0);

    /* --- escape verwirft den entwurf --- */
    keys_unread("\x10", 1); /* ctrl+p: history, egal welche zeile */
    handle_key(&st, &cfg, &dbg, 24, 80);
    CHECK(strcmp(st.input.lines[0], "neue frage") == 0);
    keys_unread("\x1b", 1); /* escape */
    handle_key(&st, &cfg, &dbg, 24, 80);
    CHECK(st.input.lines[0][0] == '\0');
    CHECK(st.history.pos == 0);
    CHECK(st.history.draft == NULL);

    input_free(&st.input);
    chat_free(&st.chat);
    history_free(&st.history);

    CHECK(dup2(saved, STDIN_FILENO) >= 0);
    close(saved);
    close(fds[0]);
    close(fds[1]);
}

int main(void)
{
    test_key_from_byte();
    test_key_from_escape();
    test_key_seq_len();
    test_keys_unread_fifo();
    test_abort_poll();
    test_history_keys();
    test_key_read_batch();
    return test_report();
}