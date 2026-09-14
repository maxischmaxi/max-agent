#include <string.h>

#include "test.h"
#include "theme.h"

static void test_parse_color_reply(void)
{
    unsigned rgb[3];

    /* 4-stellige kanaele (16 bit, wie xterm/kitty antworten) */
    CHECK(theme_parse_color_reply("11;rgb:0000/0000/0000", rgb));
    CHECK(rgb[0] == 0 && rgb[1] == 0 && rgb[2] == 0);

    CHECK(theme_parse_color_reply("11;rgb:ffff/ffff/ffff", rgb));
    CHECK(rgb[0] == 255 && rgb[1] == 255 && rgb[2] == 255);

    CHECK(theme_parse_color_reply("10;rgb:1c1c/2b2b/1616", rgb));
    CHECK(rgb[0] == 28 && rgb[1] == 43 && rgb[2] == 22);

    /* 2-stellige kanaele */
    CHECK(theme_parse_color_reply("11;rgb:1c/2b/16", rgb));
    CHECK(rgb[0] == 28 && rgb[1] == 43 && rgb[2] == 22);

    /* 1-stellige kanaele */
    CHECK(theme_parse_color_reply("10;rgb:f/0/8", rgb));
    CHECK(rgb[0] == 255 && rgb[1] == 0 && rgb[2] == 136);

    /* rgba: alpha (letzter kanal) wird ignoriert */
    CHECK(theme_parse_color_reply("11;rgba:12/34/56/ff", rgb));
    CHECK(rgb[0] == 0x12 && rgb[1] == 0x34 && rgb[2] == 0x56);

    /* ungueltige antworten */
    CHECK(!theme_parse_color_reply("11;rgb:xxxx/yyyy/zzzz", rgb));
    CHECK(
        !theme_parse_color_reply("12;rgb:ff/ff/ff", rgb)); /* falscher index */
    CHECK(!theme_parse_color_reply("11;cmyk:0/0/0/0", rgb));
    CHECK(!theme_parse_color_reply("hallo", rgb));
    CHECK(!theme_parse_color_reply("", rgb));
    CHECK(!theme_parse_color_reply(NULL, rgb));
}

static void test_set_get(void)
{
    const Theme *cur = theme_current();
    CHECK(cur != NULL && cur->name != NULL && cur->match != NULL &&
          cur->reset != NULL);

    /* eigenes theme setzen und abfragen */
    Theme custom = {"custom", "\x1b[91m", "\x1b[39m"};
    theme_set(&custom);
    CHECK(strcmp(theme_current()->name, "custom") == 0);
    CHECK(strcmp(theme_current()->match, "\x1b[91m") == 0);
    CHECK(strcmp(theme_current()->reset, "\x1b[39m") == 0);

    /* NULL bleibt ohne effekt */
    const Theme *before = theme_current();
    theme_set(NULL);
    CHECK(theme_current() == before);

    /* zurueck auf default */
    Theme def = {"auto", "\x1b[36m", "\x1b[39m"};
    theme_set(&def);
    CHECK(strcmp(theme_current()->name, "auto") == 0);
}

int main(void)
{
    test_parse_color_reply();
    test_set_get();
    return test_report();
}