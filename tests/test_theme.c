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
    Theme custom = {"custom", "\x1b[91m", "\x1b[39m", {0}};
    theme_set(&custom);
    CHECK(strcmp(theme_current()->name, "custom") == 0);
    CHECK(strcmp(theme_current()->match, "\x1b[91m") == 0);
    CHECK(strcmp(theme_current()->reset, "\x1b[39m") == 0);

    /* NULL bleibt ohne effekt */
    const Theme *before = theme_current();
    theme_set(NULL);
    CHECK(theme_current() == before);

    /* zurueck auf default */
    Theme def = {"auto", "\x1b[36m", "\x1b[39m", {0}};
    theme_set(&def);
    CHECK(strcmp(theme_current()->name, "auto") == 0);
}

static void test_select_and_options(void)
{
    /* auswahl-tabelle: index 0 = "auto", danach die benannten themes */
    CHECK(theme_option_count() >= 2);
    CHECK(theme_option_name(0) != NULL);
    CHECK(strcmp(theme_option_name(0), "auto") == 0);
    CHECK(theme_option_name(-1) == NULL);
    CHECK(theme_option_name(theme_option_count()) == NULL);

    /* alle tabellen-namen sind anwaehlbar */
    for (int i = 0; i < theme_option_count(); i++) {
        CHECK(theme_select(theme_option_name(i)));
    }

    /* benanntes theme: name + match-color wechseln */
    CHECK(theme_select("catppuccin"));
    CHECK(strcmp(theme_current()->name, "catppuccin") == 0);
    CHECK(theme_current()->match != NULL);

    /* "auto" laesst den namen auf auto zurueckkehren */
    CHECK(theme_select("auto"));
    CHECK(strcmp(theme_current()->name, "auto") == 0);

    /* unbekannt -> nichts aendern sich */
    const Theme *before = theme_current();
    CHECK(!theme_select("gibts-nicht"));
    CHECK(theme_current() == before);
    CHECK(!theme_select(NULL));
    CHECK(theme_current() == before);
}

/* rollen-farben: jede rolle liefert immer eine sequenz, themes
 * duerfen abweichen, und "auto" leitet aus der terminal-helligkeit
 * ab. */
static void test_roles(void)
{
    CHECK(theme_select("auto"));

    /* nie NULL – der renderer schreibt das ergebnis ungeprueft */
    for (int i = 0; i < THEME_ROLE_COUNT; i++) {
        CHECK(theme_role((ThemeRole)i) != NULL);
    }
    /* auch ausserhalb des gueltigen bereichs */
    CHECK(theme_role((ThemeRole)-1) != NULL);
    CHECK(theme_role(THEME_ROLE_COUNT) != NULL);
    CHECK(theme_role((ThemeRole)999) != NULL);

    /* assistant folgt der akzentfarbe des themes */
    CHECK(strcmp(theme_role(THEME_ROLE_ASSISTANT), theme_current()->match) ==
          0);
    CHECK(theme_select("dracula"));
    CHECK(strcmp(theme_role(THEME_ROLE_ASSISTANT), theme_current()->match) ==
          0);

    /* die defaults stehen: user bold, tool/notice/dim faint */
    CHECK(strcmp(theme_role(THEME_ROLE_USER), "\x1b[1m") == 0);
    CHECK(strcmp(theme_role(THEME_ROLE_TOOL), "\x1b[2m") == 0);
    CHECK(strcmp(theme_role(THEME_ROLE_NOTICE), "\x1b[2m") == 0);
    CHECK(strcmp(theme_role(THEME_ROLE_DIM), "\x1b[2m") == 0);
    /* system bleibt bewusst unauffaellig: leere sequenz */
    CHECK(theme_role(THEME_ROLE_SYSTEM)[0] == '\0');

    /* ein benanntes theme darf abweichen – und das muss beim
     * wechsel auch wieder verschwinden */
    CHECK(theme_select("catppuccin"));
    const char *cat_err = theme_role(THEME_ROLE_ERROR);
    CHECK(strcmp(cat_err, "\x1b[91m") == 0);
    CHECK(theme_select("dracula"));
    CHECK(strcmp(theme_role(THEME_ROLE_ERROR), "\x1b[31m") == 0);

    /* alle benannten themes liefern fuer jede rolle etwas */
    for (int i = 1; i < theme_option_count(); i++) {
        CHECK(theme_select(theme_option_name(i)));
        for (int rr = 0; rr < THEME_ROLE_COUNT; rr++) {
            CHECK(theme_role((ThemeRole)rr) != NULL);
        }
        /* fehler-farbe ist gesetzt und nicht die leere sequenz */
        CHECK(theme_role(THEME_ROLE_ERROR)[0] != '\0');
    }

    /* ein selbst gesetztes theme ohne rollen faellt auf die
     * defaults zurueck */
    Theme plain = {"plain", "\x1b[35m", "\x1b[39m", {0}};
    theme_set(&plain);
    CHECK(strcmp(theme_role(THEME_ROLE_USER), "\x1b[1m") == 0);
    CHECK(strcmp(theme_role(THEME_ROLE_ERROR), "\x1b[31m") == 0);
    CHECK(strcmp(theme_role(THEME_ROLE_ASSISTANT), "\x1b[35m") == 0);

    /* ... und ein theme mit eigener rolle gewinnt */
    Theme loud = {"loud", "\x1b[35m", "\x1b[39m", {0}};
    loud.roles[THEME_ROLE_USER] = "\x1b[4m"; /* unterstrichen */
    theme_set(&loud);
    CHECK(strcmp(theme_role(THEME_ROLE_USER), "\x1b[4m") == 0);
    CHECK(strcmp(theme_role(THEME_ROLE_TOOL), "\x1b[2m") == 0); /* rest */

    /* zurueck auf auto: die eigenen rollen duerfen nicht
     * haengenbleiben */
    CHECK(theme_select("auto"));
    CHECK(strcmp(theme_role(THEME_ROLE_USER), "\x1b[1m") == 0);
}

int main(void)
{
    test_parse_color_reply();
    test_set_get();
    test_select_and_options();
    test_roles();
    return test_report();
}