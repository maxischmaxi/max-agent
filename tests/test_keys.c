#include <string.h>

#include "keys.h"
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
    /* unbenannte CSI-sequenz: schlucken statt muell */
    CHECK(key_from_escape("\x1b[A", 3).kind == KEY_NONE);
    CHECK(key_from_escape("\x1b[Z", 3).kind == KEY_NONE);
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

int main(void)
{
    test_key_from_byte();
    test_key_from_escape();
    test_keys_unread_fifo();
    return test_report();
}