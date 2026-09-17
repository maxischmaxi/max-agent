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
    input_char(&in, 'a');
    input_char(&in, 'b');
    input_char(&in, 'c');
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
    input_char(&in, 'a');
    input_char(&in, 'b');
    input_char(&in, 'c');
    input_cursor_home(&in);
    CHECK(input_cursor_right(&in));
    input_char(&in, 'X');
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

    input_char(&in, 'x');
    input_char(&in, 'y');

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
    input_char(&in, 'a');
    input_char(&in, 'b');
    input_newline(&in, 24, 0, false);
    input_char(&in, 'c');
    input_char(&in, 'd');
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
    input_char(&in, 'a');
    input_char(&in, 'b');
    input_char(&in, 'c');
    input_char(&in, 'd');
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
    input_char(&in, 'a');
    input_char(&in, 'b');
    input_newline(&in, 24, 0, false);
    input_char(&in, 'c');
    input_char(&in, 'd');
    input_cursor_home(&in);

    /* backspace am zeilenanfang: umbruch loeschen -> "abcd" */
    input_backspace(&in);
    CHECK(in.count == 1);
    CHECK(strcmp(in.lines[0], "abcd") == 0);
    CHECK(in.cursor == 2); /* an der ehemaligen grenze */

    /* forward-delete am zeilenende loescht den umbruch dahinter */
    input_reset(&in);
    input_char(&in, 'a');
    input_char(&in, 'b');
    input_newline(&in, 24, 0, false);
    input_char(&in, 'c');
    input_char(&in, 'd');
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
    input_char(&in, 'a');
    input_char(&in, 'b');
    input_char(&in, 'c');
    input_char(&in, 'd');
    input_cursor_set(&in, 2);
    CHECK(input_kill_line(&in));
    CHECK(strcmp(in.lines[0], "cd") == 0);
    CHECK(in.cursor == 0);
    CHECK(!input_kill_line(&in)); /* am anfang: nichts */

    /* ctrl+w: unix-word-rubout (readline-semantik). das wort hinter
     * dem cursor stirbt, das whitespace DAVOR bleibt stehen – erst
     * der naechste druck killt es mit dem naechsten wort */
    input_reset(&in);
    input_char(&in, 'f');
    input_char(&in, 'o');
    input_char(&in, 'o');
    input_char(&in, ' ');
    input_char(&in, 'b');
    input_char(&in, 'a');
    input_char(&in, 'r');
    CHECK(input_kill_last_word(&in));
    CHECK(strcmp(in.lines[0], "foo ") == 0);
    CHECK(in.cursor == 4);
    CHECK(input_kill_last_word(&in)); /* "foo " komplett weg */
    CHECK(strcmp(in.lines[0], "") == 0);
    CHECK(in.cursor == 0);
    CHECK(!input_kill_last_word(&in)); /* anfang, keine vorherige zeile */

    /* ctrl+k: bis zeilenende */
    input_reset(&in);
    input_char(&in, 'x');
    input_char(&in, 'y');
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
    input_char(&in, 'a');
    input_char(&in, 'b');
    input_newline(&in, 24, 0, false);
    input_char(&in, 'c');
    input_char(&in, 'd');
    input_cursor_home(&in);
    CHECK(input_kill_last_word(&in));
    CHECK(in.count == 1);
    CHECK(strcmp(in.lines[0], "abcd") == 0);
    CHECK(in.cursor == 2);

    /* ctrl+k am ende von zeile 0 loescht den umbruch dahinter */
    input_cursor_end(&in); /* ans ende von "abcd" */
    input_newline(&in, 24, 0, false);
    input_char(&in, 'e');
    input_char(&in, 'f');
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
        input_char(&in, *p);
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
    input_char(&in, 'a');
    input_char(&in, 'b');
    input_newline(&in, 24, 0, false);
    input_char(&in, 'c');
    input_char(&in, 'd');
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
    input_char(&in, 'f');
    input_char(&in, 'o');
    input_char(&in, 'o');
    input_char(&in, ' ');
    input_char(&in, 'b');
    input_char(&in, 'a');
    input_char(&in, 'r');
    input_char(&in, '-');
    input_char(&in, '4');
    input_char(&in, '2');
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
    input_char(&in, 'a');
    input_char(&in, 'b');
    input_char(&in, 'c');
    input_char(&in, 'd');
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
    input_char(&in, 'o');
    input_char(&in, 'n');
    input_char(&in, 'e');
    input_char(&in, ' ');
    input_char(&in, 't');
    input_char(&in, 'w');
    input_char(&in, 'o');
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
        input_char(&in, *p);
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

/* soft-wrap: lange logische zeilen werden beim ZEICHNEN auf
 * mehrere bildschirmzeilen verteilt. der text bleibt dabei
 * unveraendert – das ist der ganze punkt der trennung. */
/* der ausloeser fuer den soft-wrap: frueher hat input_char am
 * zeilenende einfach aufgehoert, zeichen zu uebernehmen. jetzt
 * laeuft der text weiter und bricht beim zeichnen um. */
static void test_typing_past_edge(void)
{
    Input in;
    input_init(&in);

    for (int i = 0; i < 200; i++) {
        input_char(&in, (char)('a' + (i % 26)));
    }
    CHECK(strlen(in.lines[0]) == 200); /* nichts verschluckt */
    CHECK(in.count == 1);              /* logisch weiter EINE zeile */
    CHECK(in.cursor == 200);

    /* sichtbar sind es mehrere zeilen, je nach fensterbreite */
    CHECK(input_screen_rows(&in, 20) == 10);
    CHECK(input_screen_rows(&in, 50) == 4);
    CHECK(input_screen_rows(&in, 200) == 1);

    /* die obergrenze je logischer zeile greift trotzdem */
    input_reset(&in);
    for (size_t i = 0; i < INPUT_MAX_LINE_BYTES + 100; i++) {
        input_char(&in, 'z');
    }
    CHECK(strlen(in.lines[0]) == INPUT_MAX_LINE_BYTES);

    /* shift+enter bleibt davon unberuehrt: es trennt weiterhin
     * logische zeilen, egal wie breit das fenster ist */
    input_reset(&in);
    for (int i = 0; i < 100; i++) {
        input_char(&in, 'a');
    }
    input_newline(&in, 24, 0, false);
    for (int i = 0; i < 100; i++) {
        input_char(&in, 'b');
    }
    CHECK(in.count == 2);
    CHECK(strlen(in.lines[0]) == 100);
    CHECK(strlen(in.lines[1]) == 100);
    /* beide zeilen brechen fuer sich um */
    CHECK(input_screen_rows(&in, 25) == 8);

    input_free(&in);
}

static void test_screen_wrap(void)
{
    Input in;
    input_init(&in);

    /* --- leeres feld: genau eine (leere) bildschirmzeile --- */
    CHECK(input_screen_rows(&in, 10) == 1);
    size_t line = 99;
    size_t off = 99;
    size_t len = 99;
    CHECK(input_screen_row(&in, 10, 0, &line, &off, &len));
    CHECK(line == 0 && off == 0 && len == 0);
    CHECK(!input_screen_row(&in, 10, 1, NULL, NULL, NULL));

    /* --- kurzer text passt in eine zeile --- */
    input_set_text(&in, "hallo");
    CHECK(input_screen_rows(&in, 10) == 1);
    CHECK(input_screen_rows(&in, 5) == 1); /* exakt voll */
    CHECK(input_screen_rows(&in, 4) == 2); /* eins zu breit */

    /* --- umbruch an der kante, abschnitte luecken- und
     *     ueberlappungsfrei --- */
    input_set_text(&in, "abcdefghij"); /* 10 zeichen */
    CHECK(input_screen_rows(&in, 4) == 3);
    CHECK(input_screen_row(&in, 4, 0, &line, &off, &len));
    CHECK(line == 0 && off == 0 && len == 4);
    CHECK(input_screen_row(&in, 4, 1, &line, &off, &len));
    CHECK(line == 0 && off == 4 && len == 4);
    CHECK(input_screen_row(&in, 4, 2, &line, &off, &len));
    CHECK(line == 0 && off == 8 && len == 2);
    CHECK(!input_screen_row(&in, 4, 3, NULL, NULL, NULL));

    /* --- der text selbst bleibt unangetastet --- */
    CHECK(in.count == 1);
    CHECK(strcmp(in.lines[0], "abcdefghij") == 0);

    /* --- resize: andere breite, andere zeilenzahl, gleicher text --- */
    CHECK(input_screen_rows(&in, 10) == 1);
    CHECK(input_screen_rows(&in, 3) == 4);
    CHECK(input_screen_rows(&in, 1) == 10);
    CHECK(input_screen_rows(&in, 0) == 10); /* breite 0 -> wie 1 */
    CHECK(strcmp(in.lines[0], "abcdefghij") == 0);

    /* --- mehrere logische zeilen: jede bricht fuer sich um --- */
    input_set_text(&in, "abcdef\nxy\n");
    CHECK(in.count == 3);
    CHECK(input_screen_rows(&in, 4) == 4); /* 2 + 1 + 1 (leere) */
    CHECK(input_screen_row(&in, 4, 0, &line, &off, &len));
    CHECK(line == 0 && off == 0 && len == 4);
    CHECK(input_screen_row(&in, 4, 1, &line, &off, &len));
    CHECK(line == 0 && off == 4 && len == 2);
    CHECK(input_screen_row(&in, 4, 2, &line, &off, &len));
    CHECK(line == 1 && off == 0 && len == 2);
    CHECK(input_screen_row(&in, 4, 3, &line, &off, &len));
    CHECK(line == 2 && off == 0 && len == 0); /* die leere zeile */

    /* --- wort-umbruch: ganze woerter wandern in die neue zeile,
     *     ueberlange brechen hart an der kante. die abschnitte
     *     enden VOR dem space (es faellt aus der anzeige) --- */
    input_set_text(&in, "eins zwei drei");
    CHECK(input_screen_rows(&in, 10) == 2);
    CHECK(input_screen_row(&in, 10, 0, &line, &off, &len));
    CHECK(line == 0 && off == 0 && len == 9); /* "eins zwei" */
    CHECK(input_screen_row(&in, 10, 1, &line, &off, &len));
    CHECK(line == 0 && off == 10 && len == 4); /* "drei" */

    /* wort haengt ueber die kante: bricht beim space, nicht mittendrin */
    input_set_text(&in, "eins zweixyz");
    CHECK(input_screen_rows(&in, 5) == 3);
    CHECK(input_screen_row(&in, 5, 0, &line, &off, &len));
    CHECK(line == 0 && off == 0 && len == 4); /* "eins" */
    CHECK(input_screen_row(&in, 5, 1, &line, &off, &len));
    CHECK(line == 0 && off == 5 && len == 5); /* "zweix" */

    /* space-run am umbruch faellt ganz weg */
    input_set_text(&in, "ab    cd");
    CHECK(input_screen_rows(&in, 2) == 2);
    CHECK(input_screen_row(&in, 2, 0, &line, &off, &len));
    CHECK(len == 2); /* "ab", spaces fallen weg */
    CHECK(input_screen_row(&in, 2, 1, &line, &off, &len));
    CHECK(off == 6 && len == 2); /* "cd" */

    /* --- utf-8 wird nie zerschnitten --- */
    input_set_text(&in, "\xC3\xA4\xC3\xB6\xC3\xBC\xC3\x9F"); /* aeoeuess */
    CHECK(strlen(in.lines[0]) == 8);       /* 4 zeichen, 8 bytes */
    CHECK(input_screen_rows(&in, 2) == 2); /* 2 zeichen je zeile */
    CHECK(input_screen_row(&in, 2, 0, &line, &off, &len));
    CHECK(off == 0 && len == 4); /* zwei zeichen = vier bytes */
    CHECK(input_screen_row(&in, 2, 1, &line, &off, &len));
    CHECK(off == 4 && len == 4);

    input_free(&in);
}

static void test_screen_cursor(void)
{
    Input in;
    input_init(&in);

    size_t row = 99;
    size_t col = 99;

    /* leeres feld: oben links */
    input_cursor_screen(&in, 4, &row, &col);
    CHECK(row == 0 && col == 0);

    /* cursor am ende einer umgebrochenen zeile */
    input_set_text(&in, "abcdefghij"); /* cursor steht am ende */
    input_cursor_screen(&in, 4, &row, &col);
    CHECK(row == 2 && col == 2);

    /* genau an der kante: der cursor bleibt am ENDE der zeile
     * stehen (spalte w) und rutscht erst mit dem naechsten zeichen
     * weiter. dafuer ist im feld eine spalte mehr reserviert als
     * umgebrochen wird – so klebt der block sichtbar am text. */
    input_cursor_set(&in, 4);
    input_cursor_screen(&in, 4, &row, &col);
    CHECK(row == 0 && col == 4);
    input_cursor_set(&in, 5); /* ein zeichen weiter: neue zeile */
    input_cursor_screen(&in, 4, &row, &col);
    CHECK(row == 1 && col == 1);
    input_cursor_set(&in, 3);
    input_cursor_screen(&in, 4, &row, &col);
    CHECK(row == 0 && col == 3);

    /* zweite logische zeile zaehlt die erste mit */
    input_set_text(&in, "abcdefgh\nxy");
    input_cursor_screen(&in, 4, &row, &col);
    CHECK(row == 2 && col == 2); /* 2 zeilen umbruch + cursor hinter xy */

    /* --- hoch/runter ueber BILDSCHIRMzeilen --- */
    input_set_text(&in, "abcdefghij"); /* eine logische, drei sichtbare */
    input_cursor_set(&in, 9);
    input_cursor_screen(&in, 4, &row, &col);
    CHECK(row == 2 && col == 1);

    CHECK(input_screen_up(&in, 4));
    input_cursor_screen(&in, 4, &row, &col);
    CHECK(row == 1 && col == 1);
    CHECK(in.cursor_line == 0); /* immer noch dieselbe logische zeile */
    CHECK(in.cursor == 5);

    CHECK(input_screen_up(&in, 4));
    input_cursor_screen(&in, 4, &row, &col);
    CHECK(row == 0 && col == 1);

    CHECK(!input_screen_up(&in, 4)); /* oben angekommen */

    CHECK(input_screen_down(&in, 4));
    input_cursor_screen(&in, 4, &row, &col);
    CHECK(row == 1 && col == 1);
    CHECK(input_screen_down(&in, 4));
    CHECK(!input_screen_down(&in, 4)); /* unten angekommen */

    /* kuerzere zielzeile: die spalte wird geklemmt */
    input_set_text(&in, "abcd\nx");
    input_cursor_set(&in, 1); /* in zeile 2, spalte 1 */
    CHECK(input_screen_up(&in, 4));
    input_cursor_screen(&in, 4, &row, &col);
    CHECK(row == 0 && col == 1);
    input_cursor_set(&in, 4); /* ende der ersten zeile */
    in.cursor_line = 0;
    CHECK(input_screen_down(&in, 4));
    CHECK(in.cursor_line == 1);
    CHECK(in.cursor <= strlen(in.lines[1])); /* geklemmt */

    /* NULL ist ueberall zulaessig */
    input_cursor_screen(NULL, 4, &row, &col);
    CHECK(row == 0 && col == 0);
    CHECK(input_screen_rows(NULL, 4) == 0);
    CHECK(!input_screen_row(NULL, 4, 0, NULL, NULL, NULL));

    input_free(&in);
}

/* der gemeldete bug: umlaute sind im wrap-plan 1 zelle, im alten
 * renderer 2 zellen – die letzten zeichen fielen vom zeilenrand,
 * der cursor-block verschwand. plan und abschnitte muessen in
 * ZELLEN (codepoints) denken, nicht bytes: 4 umlaute passen in
 * ein 4-zellen-feld exakt, 5 brauchen zwei zeilen. */
static void test_screen_wrap_utf8(void)
{
    Input in;
    input_init(&in);

    /* 4 umlaute = 4 zellen: passt in EINE zeile */
    input_set_text(&in, "\xC3\xA4\xC3\xB6\xC3\xBC\xC3\x9F");
    CHECK(input_screen_rows(&in, 4) == 1);

    /* 5 umlaute = 5 zellen: umbruch nach 4 (8 bytes) */
    input_set_text(&in, "\xC3\xA4\xC3\xA4\xC3\xA4\xC3\xA4\xC3\xA4");
    CHECK(input_screen_rows(&in, 4) == 2);
    size_t line = 99;
    size_t off = 99;
    size_t len = 99;
    CHECK(input_screen_row(&in, 4, 0, &line, &off, &len));
    CHECK(off == 0 && len == 8);
    CHECK(input_screen_row(&in, 4, 1, &line, &off, &len));
    CHECK(off == 8 && len == 2);

    /* wortumbruch mit umlauten: a-uml+"ns"+o-uml+"hn" (7 zellen)
     * passt exakt, u-uml+"ber" wandert als ganzes wort in zeile 2.
     * (die oktalen escapes verhindern, dass der C-lexer hex-escape
     * und folgebuchstaben vermischt: "\xBCb" waere EIN token) */
    input_set_text(&in, "\303\244ns\303\266hn \303\274ber");
    CHECK(input_screen_rows(&in, 7) == 2);
    CHECK(input_screen_row(&in, 7, 0, &line, &off, &len));
    CHECK(off == 0 && len == 8); /* 8 bytes: a-uml n s o-uml h n */
    CHECK(input_screen_row(&in, 7, 1, &line, &off, &len));
    CHECK(off == 9 && len == 5); /* u-uml b e r = 5 bytes */

    input_free(&in);
}

/* utf-8-editierung: backspace, delete und cursor arbeiten auf
 * ZEICHEN, nicht auf bytes – ein umlaut ist eine einheit, ein
 * halbes wuerde kaputte sequenzen im feld hinterlassen */
static void test_utf8_editing(void)
{
    Input in;
    input_init(&in);

    /* backspace hinter dem umlaut nimmt das GANZE zeichen */
    input_set_text(&in, "h\xC3\xA4llo");
    in.cursor_line = 0;
    in.cursor = 3; /* hinter 'ae' (h + 2 bytes) */
    input_backspace(&in);
    CHECK(strcmp(in.lines[0], "hllo") == 0);
    CHECK(in.cursor == 1);

    /* backspace ueber einem emoji (4 bytes) */
    input_set_text(&in, "\xF0\x9F\x98\x80x");
    in.cursor_line = 0;
    in.cursor = 4; /* hinter dem emoji */
    input_backspace(&in);
    CHECK(strcmp(in.lines[0], "x") == 0);
    CHECK(in.cursor == 0);

    /* cursor links: einmal vor das GANZE umlaut (nicht mitten rein) */
    input_set_text(&in, "\xC3\xA4l");
    in.cursor_line = 0;
    in.cursor = 3; /* zeilenende */
    CHECK(input_cursor_left(&in));
    CHECK(in.cursor == 2); /* vor 'l' */
    CHECK(input_cursor_left(&in));
    CHECK(in.cursor == 0); /* vor dem umlaut, nicht byte 1 */

    /* cursor rechts: um das ganze zeichen */
    input_set_text(&in, "\xC3\xA4l");
    in.cursor_line = 0;
    in.cursor = 0;
    CHECK(input_cursor_right(&in));
    CHECK(in.cursor == 2); /* hinter dem umlaut */

    /* delete forward unter dem umlaut loescht es komplett */
    input_set_text(&in, "x\xC3\xA4y");
    in.cursor_line = 0;
    in.cursor = 1;
    CHECK(input_delete_forward(&in));
    CHECK(strcmp(in.lines[0], "xy") == 0);
    CHECK(in.cursor == 1);

    /* ascii bleibt 1:1 (kein verhalten kaputt) */
    input_set_text(&in, "ab");
    in.cursor_line = 0;
    in.cursor = 1;
    input_backspace(&in);
    CHECK(strcmp(in.lines[0], "b") == 0);
    CHECK(in.cursor == 0);

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
    test_typing_past_edge();
    test_screen_wrap();
    test_screen_wrap_utf8();
    test_screen_cursor();
    test_utf8_editing();
    return test_report();
}