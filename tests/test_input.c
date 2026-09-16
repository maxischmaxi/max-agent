#include <string.h>

#include "input.h"
#include "test.h"

static void test_cursor_utility(void)
{
    Input in;
    input_init(&in);

    CHECK(in.cursor == 0);
    CHECK(input_len(&in) == 0);

    /* tippen bewegt den cursor mit (anhaengen am ende) */
    input_char(&in, 'a', 80);
    input_char(&in, 'b', 80);
    input_char(&in, 'c', 80);
    CHECK(input_len(&in) == 3);
    CHECK(in.cursor == 3);

    /* home/end */
    input_cursor_home(&in);
    CHECK(in.cursor == 0);
    input_cursor_end(&in);
    CHECK(in.cursor == 3);

    /* links/rechts mit randerkennung */
    CHECK(input_cursor_left(&in));
    CHECK(input_cursor_left(&in));
    CHECK(input_cursor_left(&in));
    CHECK(!input_cursor_left(&in)); /* anfang erreicht */
    CHECK(in.cursor == 0);
    CHECK(input_cursor_right(&in));
    CHECK(input_cursor_right(&in));
    CHECK(input_cursor_right(&in));
    CHECK(!input_cursor_right(&in)); /* ende erreicht */
    CHECK(in.cursor == 3);

    /* set klemmt auf 0..len */
    input_cursor_set(&in, 99);
    CHECK(in.cursor == 3);
    input_cursor_set(&in, 1);
    CHECK(in.cursor == 1);

    input_free(&in);
}

static void test_insert_at_cursor(void)
{
    Input in;
    input_init(&in);

    /* "abc", cursor auf 1 -> 'X' wird MITTEN eingefuegt */
    input_char(&in, 'a', 80);
    input_char(&in, 'b', 80);
    input_char(&in, 'c', 80);
    input_cursor_home(&in);
    CHECK(input_cursor_right(&in));
    input_char(&in, 'X', 80);
    CHECK(strcmp(in.lines[in.count - 1], "aXbc") == 0);
    CHECK(in.cursor == 2);

    /* backspace loescht das zeichen VOR dem cursor */
    input_backspace(&in);
    CHECK(strcmp(in.lines[in.count - 1], "abc") == 0);
    CHECK(in.cursor == 1);

    /* backspace am zeilenanfang: nichts loeschen */
    input_cursor_home(&in);
    input_backspace(&in);
    CHECK(strcmp(in.lines[in.count - 1], "abc") == 0);
    CHECK(in.cursor == 0);

    /* backspace am ende = klassisches verhalten */
    input_cursor_end(&in);
    input_backspace(&in);
    CHECK(strcmp(in.lines[in.count - 1], "ab") == 0);
    CHECK(in.cursor == 2);

    input_free(&in);
}

static void test_newline_and_line_removal(void)
{
    Input in;
    input_init(&in);

    input_char(&in, 'x', 80);
    input_char(&in, 'y', 80);

    /* neue zeile: cursor an deren anfang */
    input_newline(&in, 24, 0, false);
    CHECK(in.count == 2);
    CHECK(in.cursor_line == 1);
    CHECK(in.cursor == 0);
    CHECK(input_len(&in) == 0);

    /* backspace auf leerer letzter zeile: zeile weg, cursor am
     * ende der zeile davor (dorthin kam man her) */
    input_backspace(&in);
    CHECK(in.count == 1);
    CHECK(strcmp(in.lines[0], "xy") == 0);
    CHECK(in.cursor == 2);

    /* reset: alles leer, cursor 0 */
    input_reset(&in);
    CHECK(in.count == 1);
    CHECK(in.cursor_line == 0);
    CHECK(in.cursor == 0);
    CHECK(input_len(&in) == 0);

    input_free(&in);
}

static void test_multiline_movement(void)
{
    Input in;
    input_init(&in);

    /* zwei zeilen: "ab" / "cd" */
    input_char(&in, 'a', 80);
    input_char(&in, 'b', 80);
    input_newline(&in, 24, 0, false);
    input_char(&in, 'c', 80);
    input_char(&in, 'd', 80);
    CHECK(in.count == 2);

    /* links ueber die zeilengrenze: zeile 1 anfang -> zeile 0 ende */
    input_cursor_home(&in);
    CHECK(input_cursor_left(&in));
    CHECK(in.cursor_line == 0);
    CHECK(in.cursor == 2);          /* ende von zeile 0 */
    CHECK(input_cursor_left(&in));  /* innerhalb zeile 0 weiter */
    CHECK(input_cursor_left(&in));  /* cursor 0 */
    CHECK(!input_cursor_left(&in)); /* jetzt anfang von allem */
    CHECK(!input_cursor_left(&in)); /* idempotent */

    /* rechts wieder bis ueber die grenze */
    CHECK(input_cursor_right(&in));
    CHECK(input_cursor_right(&in));
    CHECK(input_cursor_right(&in)); /* grenze: anfang von zeile 1 */
    CHECK(in.cursor_line == 1);
    CHECK(in.cursor == 0);
    input_cursor_end(&in);
    CHECK(!input_cursor_right(&in)); /* ende von allem */

    input_free(&in);
}

static void test_newline_splits_at_cursor(void)
{
    Input in;
    input_init(&in);

    /* "abcd", cursor auf 2 -> split in "ab" / "cd" */
    input_char(&in, 'a', 80);
    input_char(&in, 'b', 80);
    input_char(&in, 'c', 80);
    input_char(&in, 'd', 80);
    input_cursor_set(&in, 2);
    input_newline(&in, 24, 0, false);
    CHECK(in.count == 2);
    CHECK(strcmp(in.lines[0], "ab") == 0);
    CHECK(strcmp(in.lines[1], "cd") == 0);
    CHECK(in.cursor_line == 1);
    CHECK(in.cursor == 0);

    input_free(&in);
}

static void test_join_and_forward_delete(void)
{
    Input in;
    input_init(&in);

    /* "ab" / "cd", cursor am anfang von zeile 1 */
    input_char(&in, 'a', 80);
    input_char(&in, 'b', 80);
    input_newline(&in, 24, 0, false);
    input_char(&in, 'c', 80);
    input_char(&in, 'd', 80);
    input_cursor_home(&in);

    /* backspace am zeilenanfang: umbruch loeschen -> "abcd" */
    input_backspace(&in);
    CHECK(in.count == 1);
    CHECK(strcmp(in.lines[0], "abcd") == 0);
    CHECK(in.cursor == 2); /* an der ehemaligen grenze */

    /* forward-delete am zeilenende loescht den umbruch dahinter */
    input_reset(&in);
    input_char(&in, 'a', 80);
    input_char(&in, 'b', 80);
    input_newline(&in, 24, 0, false);
    input_char(&in, 'c', 80);
    input_char(&in, 'd', 80);
    input_cursor_line_set(&in, 0);
    CHECK(in.cursor == 2); /* klemmt am ende von zeile 0 */
    CHECK(input_delete_forward(&in));
    CHECK(in.count == 1);
    CHECK(strcmp(in.lines[0], "abcd") == 0);
    CHECK(in.cursor == 2); /* an der ehemaligen grenze, mitten drin */

    /* forward-delete MITTEN in der zeile (cursor steht auf 2) */
    CHECK(input_delete_forward(&in));
    CHECK(strcmp(in.lines[0], "abd") == 0);

    /* am ende von allem: nichts mehr */
    input_cursor_end(&in);
    CHECK(!input_delete_forward(&in));

    input_free(&in);
}

static void test_kill_variants(void)
{
    Input in;
    input_init(&in);

    /* ctrl+u: von anfang bis cursor */
    input_char(&in, 'a', 80);
    input_char(&in, 'b', 80);
    input_char(&in, 'c', 80);
    input_char(&in, 'd', 80);
    input_cursor_set(&in, 2);
    CHECK(input_kill_line(&in));
    CHECK(strcmp(in.lines[0], "cd") == 0);
    CHECK(in.cursor == 0);
    CHECK(!input_kill_line(&in)); /* am anfang: nichts */

    /* ctrl+w: unix-word-rubout (readline-semantik). das wort hinter
     * dem cursor stirbt, das whitespace DAVOR bleibt stehen – erst
     * der naechste druck killt es mit dem naechsten wort */
    input_reset(&in);
    input_char(&in, 'f', 80);
    input_char(&in, 'o', 80);
    input_char(&in, 'o', 80);
    input_char(&in, ' ', 80);
    input_char(&in, 'b', 80);
    input_char(&in, 'a', 80);
    input_char(&in, 'r', 80);
    CHECK(input_kill_last_word(&in));
    CHECK(strcmp(in.lines[0], "foo ") == 0);
    CHECK(in.cursor == 4);
    CHECK(input_kill_last_word(&in)); /* "foo " komplett weg */
    CHECK(strcmp(in.lines[0], "") == 0);
    CHECK(in.cursor == 0);
    CHECK(!input_kill_last_word(&in)); /* anfang, keine vorherige zeile */

    /* ctrl+k: bis zeilenende */
    input_reset(&in);
    input_char(&in, 'x', 80);
    input_char(&in, 'y', 80);
    input_cursor_home(&in);
    input_cursor_right(&in);
    CHECK(input_kill_to_end(&in));
    CHECK(strcmp(in.lines[0], "x") == 0);
    CHECK(in.cursor == 1);
    CHECK(!input_kill_to_end(&in)); /* am ende: nichts */

    input_free(&in);
}

static void test_kill_across_lines(void)
{
    Input in;
    input_init(&in);

    /* "ab" / "cd"; ctrl+w am anfang von zeile 1 loescht den umbruch */
    input_char(&in, 'a', 80);
    input_char(&in, 'b', 80);
    input_newline(&in, 24, 0, false);
    input_char(&in, 'c', 80);
    input_char(&in, 'd', 80);
    input_cursor_home(&in);
    CHECK(input_kill_last_word(&in));
    CHECK(in.count == 1);
    CHECK(strcmp(in.lines[0], "abcd") == 0);
    CHECK(in.cursor == 2);

    /* ctrl+k am ende von zeile 0 loescht den umbruch dahinter */
    input_cursor_end(&in); /* ans ende von "abcd" */
    input_newline(&in, 24, 0, false);
    input_char(&in, 'e', 80);
    input_char(&in, 'f', 80);
    CHECK(strcmp(in.lines[1], "ef") == 0);
    input_cursor_line_set(&in, 0);
    input_cursor_end(&in);
    CHECK(input_kill_to_end(&in));
    CHECK(in.count == 1);
    CHECK(strcmp(in.lines[0], "abcdef") == 0);

    input_free(&in);
}

static void test_word_navigation(void)
{
    Input in;
    input_init(&in);

    /* "foo bar-42 x" – alnum-woerter: foo, bar, 42, x */
    const char *text = "foo bar-42 x";
    for (const char *p = text; *p != '\0'; p++) {
        input_char(&in, *p, 80);
    }

    /* alt+b: wortweise zurueck. "x" startet bei index 11 */
    CHECK(input_word_left(&in));
    CHECK(in.cursor == 11);
    CHECK(input_word_left(&in));
    CHECK(in.cursor == 8); /* anfang von "42" */
    CHECK(input_word_left(&in));
    CHECK(in.cursor == 4); /* anfang von "bar" ('-' ist grenze) */
    CHECK(input_word_left(&in));
    CHECK(in.cursor == 0);
    CHECK(!input_word_left(&in)); /* anfang von allem */

    /* alt+f: wortweise vor. "x" endet bei index 12 */
    CHECK(input_word_right(&in));
    CHECK(in.cursor == 3); /* ende von "foo" */
    CHECK(input_word_right(&in));
    CHECK(in.cursor == 7); /* ende von "bar" ('-' uebersprungen) */
    CHECK(input_word_right(&in));
    CHECK(in.cursor == 10); /* ende von "42" */
    CHECK(input_word_right(&in));
    CHECK(in.cursor == 12);
    CHECK(!input_word_right(&in)); /* ende von allem */

    input_free(&in);
}

static void test_word_movement_across_lines(void)
{
    Input in;
    input_init(&in);

    /* "ab" / "cd" – alt+b von zeile 1 anfang */
    input_char(&in, 'a', 80);
    input_char(&in, 'b', 80);
    input_newline(&in, 24, 0, false);
    input_char(&in, 'c', 80);
    input_char(&in, 'd', 80);
    input_cursor_home(&in);

    CHECK(input_word_left(&in));
    CHECK(in.cursor_line == 0);
    CHECK(in.cursor == 0); /* anfang von "ab" in zeile 0 */
    CHECK(!input_word_left(&in));

    CHECK(input_word_right(&in));
    CHECK(in.cursor_line == 0);
    CHECK(in.cursor == 2); /* ende von "ab" */
    CHECK(input_word_right(&in));
    CHECK(in.cursor_line == 1);
    CHECK(in.cursor == 2); /* ende von "cd" – ueber die grenze */
    CHECK(!input_word_right(&in));

    input_free(&in);
}

static void test_word_kills(void)
{
    Input in;
    input_init(&in);

    /* alt+d (kill-word, vorwaerts): "foo bar-42" -> "foo -42" */
    input_char(&in, 'f', 80);
    input_char(&in, 'o', 80);
    input_char(&in, 'o', 80);
    input_char(&in, ' ', 80);
    input_char(&in, 'b', 80);
    input_char(&in, 'a', 80);
    input_char(&in, 'r', 80);
    input_char(&in, '-', 80);
    input_char(&in, '4', 80);
    input_char(&in, '2', 80);
    input_cursor_set(&in, 4);
    CHECK(input_kill_word(&in));
    CHECK(strcmp(in.lines[0], "foo -42") == 0);

    /* alt+backspace (alnum-rubout): "foo -42", cursor auf 4 ->
     * "foo " stirbt komplett (space ist non-alnum-grenze),
     * "-" bleibt vorn */
    input_cursor_set(&in, 4);
    CHECK(input_kill_word_back(&in));
    CHECK(strcmp(in.lines[0], "-42") == 0);
    CHECK(in.cursor == 0);
    /* am anfang ohne vorherige zeile: nichts */
    CHECK(!input_kill_word_back(&in));

    input_free(&in);
}

static void test_transpose(void)
{
    Input in;
    input_init(&in);

    /* ctrl+t mitten drin: "abcd", cursor auf 1 -> "bacd" */
    input_char(&in, 'a', 80);
    input_char(&in, 'b', 80);
    input_char(&in, 'c', 80);
    input_char(&in, 'd', 80);
    input_cursor_set(&in, 1);
    CHECK(input_transpose_chars(&in));
    CHECK(strcmp(in.lines[0], "bacd") == 0);
    CHECK(in.cursor == 2);

    /* ctrl+t am anfang: nichts */
    input_cursor_home(&in);
    CHECK(!input_transpose_chars(&in));

    /* ctrl+t am ende: letzte zwei tauschen */
    input_cursor_end(&in);
    CHECK(input_transpose_chars(&in));
    CHECK(strcmp(in.lines[0], "badc") == 0);

    /* alt+t: woerter tauschen: "one two", cursor nach "one" ->
     * "two one", cursor hinter dem (neu hinten liegenden) wort */
    input_reset(&in);
    input_char(&in, 'o', 80);
    input_char(&in, 'n', 80);
    input_char(&in, 'e', 80);
    input_char(&in, ' ', 80);
    input_char(&in, 't', 80);
    input_char(&in, 'w', 80);
    input_char(&in, 'o', 80);
    CHECK(input_transpose_words(&in));
    CHECK(strcmp(in.lines[0], "two one") == 0);
    CHECK(in.cursor == 7);

    input_free(&in);
}

static void test_word_case(void)
{
    Input in;
    input_init(&in);

    const char *text = "hello world";
    for (const char *p = text; *p != '\0'; p++) {
        input_char(&in, *p, 80);
    }

    /* alt+u: ganzes wort GROSS, cursor am wortende */
    input_cursor_home(&in);
    CHECK(input_word_upcase(&in));
    CHECK(strcmp(in.lines[0], "HELLO world") == 0);
    CHECK(in.cursor == 5);

    /* alt+c an der wortgrenze: greift das FOLGENDE wort (readline) */
    CHECK(input_word_capitalize(&in));
    CHECK(strcmp(in.lines[0], "HELLO World") == 0);
    CHECK(in.cursor == 11);

    /* alt+l zweimal: erst "HELLO", dann (an der grenze) "World" */
    input_cursor_home(&in);
    CHECK(input_word_downcase(&in));
    CHECK(strcmp(in.lines[0], "hello World") == 0);
    CHECK(in.cursor == 5);
    CHECK(input_word_downcase(&in));
    CHECK(strcmp(in.lines[0], "hello world") == 0);

    input_free(&in);
}

static void test_set_text(void)
{
    Input in;
    input_init(&in);

    /* einzeilig: cursor landet am ende */
    input_set_text(&in, "hallo welt");
    CHECK(in.count == 1);
    CHECK(strcmp(in.lines[0], "hallo welt") == 0);
    CHECK(in.cursor_line == 0);
    CHECK(in.cursor == strlen("hallo welt"));

    /* mehrzeilig: an '\n' zerlegt, cursor am ende der LETZTEN zeile */
    input_set_text(&in, "eins\nzwei\ndrei");
    CHECK(in.count == 3);
    CHECK(strcmp(in.lines[0], "eins") == 0);
    CHECK(strcmp(in.lines[1], "zwei") == 0);
    CHECK(strcmp(in.lines[2], "drei") == 0);
    CHECK(in.cursor_line == 2);
    CHECK(in.cursor == strlen("drei"));

    /* ersetzt wirklich, haengt nicht an */
    input_set_text(&in, "kurz");
    CHECK(in.count == 1);
    CHECK(strcmp(in.lines[0], "kurz") == 0);

    /* leere zeilen mittendrin bleiben erhalten */
    input_set_text(&in, "a\n\nb");
    CHECK(in.count == 3);
    CHECK(in.lines[1][0] == '\0');

    /* abschliessendes '\n' erzeugt eine leere letzte zeile */
    input_set_text(&in, "a\n");
    CHECK(in.count == 2);
    CHECK(strcmp(in.lines[0], "a") == 0);
    CHECK(in.lines[1][0] == '\0');
    CHECK(in.cursor_line == 1);
    CHECK(in.cursor == 0);

    /* NULL und "" leeren das feld */
    input_set_text(&in, "");
    CHECK(in.count == 1);
    CHECK(in.lines[0][0] == '\0');
    input_set_text(&in, "x\ny");
    input_set_text(&in, NULL);
    CHECK(in.count == 1);
    CHECK(in.lines[0][0] == '\0');

    /* mehr zeilen als das feld fasst: der rest faellt weg, die
     * invariante (cursor_line < count) bleibt heil */
    char many[4 * (INPUT_MAX_LINES + 8)];
    size_t pos = 0;
    for (int i = 0; i < INPUT_MAX_LINES + 8; i++) {
        many[pos++] = (char)('a' + (i % 26));
        many[pos++] = '\n';
    }
    many[pos - 1] = '\0'; /* letztes '\n' weg */
    input_set_text(&in, many);
    CHECK(in.count == INPUT_MAX_LINES);
    CHECK(in.cursor_line == in.count - 1);
    CHECK(in.cursor <= strlen(in.lines[in.cursor_line]));

    input_free(&in);
}

int main(void)
{
    test_cursor_utility();
    test_insert_at_cursor();
    test_newline_and_line_removal();
    test_multiline_movement();
    test_newline_splits_at_cursor();
    test_join_and_forward_delete();
    test_kill_variants();
    test_kill_across_lines();
    test_word_navigation();
    test_word_movement_across_lines();
    test_word_kills();
    test_transpose();
    test_word_case();
    test_set_text();
    return test_report();
}