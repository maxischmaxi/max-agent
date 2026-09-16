#include <stdlib.h>
#include <string.h>

#include "chat.h"
#include "draw.h"
#include "state.h"
#include "test.h"
#include "utils.h"

/* chat_flatten_input braucht einen echten Input; input_char stirbt
 * bei OOM (die()), daher hier von hand aufbauen */
static void input_set_line(Input *in, size_t line, const char *text)
{
    free(in->lines[line]);
    in->lines[line] = dup_str(text);
}

/* ------------------------------------------------------------------ */
/* abschnitte als eigene funktionen: clang-tidy (readability-        */
/* function-size) mag keine 900-statement-mains */
/* ------------------------------------------------------------------ */

static void test_append_pop_clear(void)
{
    /* --- grundzustand: zero-init ist ein leeres transcript --- */
    Chat chat = {0};
    CHECK(chat.msgs == NULL);
    CHECK(chat.len == 0);
    CHECK(!chat_pop(&chat));
    chat_free(&chat);
    CHECK(chat.msgs == NULL);
    CHECK(chat.cap == 0);

    /* --- anhaengen, lesen, rollen --- */
    CHECK(chat_append(&chat, CHAT_ROLE_USER, "hallo") == 0);
    CHECK(chat_append(&chat, CHAT_ROLE_ASSISTANT, "hi!") == 0);
    CHECK(chat.len == 2);
    CHECK(chat.msgs[0].role == CHAT_ROLE_USER);
    CHECK(chat.msgs[1].role == CHAT_ROLE_ASSISTANT);
    CHECK(strcmp(chat.msgs[0].text, "hallo") == 0);
    CHECK(strcmp(chat.msgs[1].text, "hi!") == 0);

    /* leerer text ist erlaubt (streaming-platzhalter) */
    CHECK(chat_append(&chat, CHAT_ROLE_ASSISTANT, "") == 0);
    CHECK(chat.len == 3);
    CHECK(chat.msgs[2].text[0] == '\0');

    /* NULL-text ist ein fehler, veraendert nichts */
    CHECK(chat_append(&chat, CHAT_ROLE_USER, NULL) == -1);
    CHECK(chat.len == 3);

    /* --- pop: letzte nachricht weg, text freigegeben --- */
    CHECK(chat_pop(&chat));
    CHECK(chat.len == 2);
    CHECK(chat.msgs[2].text == NULL);
    CHECK(chat_pop(&chat));
    CHECK(chat_pop(&chat));
    CHECK(chat.len == 0);
    CHECK(!chat_pop(&chat));

    /* --- chat_append_text: an die LETZTE nachricht anhaengen --- */
    chat_clear(&chat);
    CHECK(!chat_append_text(&chat, "text")); /* leeres chat */
    CHECK(chat_append(&chat, CHAT_ROLE_ASSISTANT, "") == 0);
    CHECK(chat_append_text(&chat, "hal")); /* platzhalter fuellen */
    CHECK(chat_append_text(&chat, ""));    /* leerer text: no-op */
    CHECK(chat_append_text(&chat, "lo"));
    CHECK(strcmp(chat.msgs[0].text, "hallo") == 0);
    CHECK(chat.msgs[0].role == CHAT_ROLE_ASSISTANT);
    CHECK(!chat_append_text(&chat, NULL));
    CHECK(strcmp(chat.msgs[0].text, "hallo") == 0); /* unveraendert */

    /* anhaengen an die letzte veraendert die anderen pointer nicht:
     * hier laeuft spaeter der stream-delta rein */
    CHECK(chat_append(&chat, CHAT_ROLE_USER, "naechste frage") == 0);
    CHECK(chat_append_text(&chat, "?"));
    CHECK(strcmp(chat.msgs[1].text, "naechste frage?") == 0);
    CHECK(strcmp(chat.msgs[0].text, "hallo") == 0);
    chat_clear(&chat);

    /* --- tool-calls: ownership + darstellungs-zeilen --- */
    ChatLine tlines[16];
    /* heap-array: chat_set_tool_calls uebernimmt den besitz und
     * gibt die felder spaeter mit chat_pop frei */
    ChatToolCall *calls = calloc(2, sizeof *calls);
    if (calls == NULL) {
        die("out of memory");
    }
    calls[0].id = dup_str("call_1");
    calls[0].name = dup_str("bash");
    calls[0].arguments = dup_str("{\"command\":\"ls\"}");
    CHECK(calls[0].id != NULL && calls[0].name != NULL);
    calls[1].id = dup_str("call_2");
    calls[1].name = dup_str("read_file");

    /* set_tool_calls braucht eine assistant-nachricht */
    CHECK(chat_append(&chat, CHAT_ROLE_USER, "frage") == 0);
    CHECK(chat_set_tool_calls(&chat, calls, 2) !=
          0); /* user, nicht assistant */
    CHECK(chat_append(&chat, CHAT_ROLE_ASSISTANT, "") == 0);
    CHECK(chat_set_tool_calls(&chat, calls, 2) == 0); /* ownership wechselt */
    CHECK(chat.msgs[1].tool_calls_len == 2);
    CHECK(chat.msgs[1].tool_calls[0].name != NULL &&
          strcmp(chat.msgs[1].tool_calls[0].name, "bash") == 0);

    /* tool-ergebnis mit call-bezug */
    CHECK(chat_append_tool(&chat, "call_1", "ergebnis") == 0);
    CHECK(chat.len == 3);
    CHECK(chat.msgs[2].role == CHAT_ROLE_TOOL);
    CHECK(chat.msgs[2].tool_call_id != NULL &&
          strcmp(chat.msgs[2].tool_call_id, "call_1") == 0);
    CHECK(strcmp(chat.msgs[2].text, "ergebnis") == 0);
    CHECK(chat_append_tool(&chat, NULL, "x") != 0); /* id ist pflicht */

    /* wrap: user-frage, leerer assistant-platzhalter, je eine
     * darstellungs-zeile pro call, dann das tool-ergebnis */
    CHECK(chat_wrap(&chat, 40, tlines, 16) == 5);
    CHECK(tlines[0].role == CHAT_ROLE_USER);
    CHECK(tlines[1].role == CHAT_ROLE_ASSISTANT);
    CHECK(tlines[1].tool == -1); /* leerer text -> eine leere zeile */
    CHECK(tlines[2].tool == 0);  /* darstellungs-zeile call 1 */
    CHECK(tlines[2].msg == 1);
    CHECK(tlines[3].tool == 1);  /* call 2 */
    CHECK(tlines[4].tool == -1); /* tool-ergebnis: normale textzeile */
    CHECK(tlines[4].role == CHAT_ROLE_TOOL);

    /* pop/clear geben alle felder frei (asan prueft das) */
    CHECK(chat_pop(&chat)); /* tool-ergebnis weg */
    CHECK(chat_pop(&chat)); /* assistant MIT calls weg */
    CHECK(chat_pop(&chat));
    CHECK(chat.len == 0);
    chat_clear(&chat);

    /* --- kapazitaet bleibt nach clear/leerwerden erhalten --- */
    CHECK(chat.cap > 0);
    CHECK(chat.msgs != NULL);

    /* --- clear: inhalt weg, struct sofort wiederverwendbar --- */
    for (int i = 0; i < 10; i++) {
        CHECK(chat_append(&chat, CHAT_ROLE_USER, "x") == 0);
    }
    CHECK(chat.len == 10);
    chat_clear(&chat);
    CHECK(chat.len == 0);
    CHECK(chat_append(&chat, CHAT_ROLE_SYSTEM, "du bist max agent.") == 0);
    CHECK(chat.len == 1);
    CHECK(strcmp(chat.msgs[0].text, "du bist max agent.") == 0);
    CHECK(chat.msgs[0].role == CHAT_ROLE_SYSTEM);

    /* --- clear laesst alte pointer unbenutzt, aber freigegeben --- */
    chat_clear(&chat);

    /* --- viele messages: wachstum/doubling ueber 8 hinaus --- */
    for (int i = 0; i < 100; i++) {
        char buf[32];
        snprintf(buf, sizeof buf, "msg %d", i);
        CHECK(chat_append(&chat, (i % 2) ? CHAT_ROLE_ASSISTANT : CHAT_ROLE_USER,
                          buf) == 0);
    }
    CHECK(chat.len == 100);
    CHECK(chat.cap >= 100);
    CHECK(strcmp(chat.msgs[99].text, "msg 99") == 0);
    CHECK(chat.msgs[51].role == CHAT_ROLE_ASSISTANT);
    chat_free(&chat);
}

static void test_flatten(void)
{
    /* --- chat_flatten_input --- */

    /* leere eingabe: nur die init-zeile, NULL erwartet */
    Input in;
    input_init(&in);
    CHECK(chat_flatten_input(&in) == NULL);

    /* eine zeile text */
    input_set_line(&in, 0, "hallo welt");
    char *flat = chat_flatten_input(&in);
    CHECK(flat != NULL && strcmp(flat, "hallo welt") == 0);
    free(flat);

    /* mehrere zeilen: '\n' dazwischen, leere zeile hinten bleibt.
     * input_newline splittet am cursor, also erst ans zeilenende. */
    input_cursor_end(&in);
    input_newline(&in, 40, 0, false);
    input_set_line(&in, 1, "zweite");
    input_cursor_end(&in);
    input_newline(&in, 40, 0, false);
    flat = chat_flatten_input(&in);
    CHECK(flat != NULL && strcmp(flat, "hallo welt\nzweite\n") == 0);
    free(flat);

    /* nur leere zeilen gilt als leere eingabe */
    input_free(&in);
    input_init(&in);
    input_newline(&in, 40, 0, false);
    input_set_line(&in, 0, "");
    input_set_line(&in, 1, "");
    CHECK(chat_flatten_input(&in) == NULL);
    input_free(&in);
}

static void test_wrap(void)
{
    /* --- chat_wrap --- */
    ChatLine lines[32] = {0};
    Chat c = {0};

    /* leeres chat: 0 zeilen */
    CHECK(chat_wrap(&c, 20, lines, 32) == 0);
    CHECK(chat_wrap(&c, 20, NULL, 0) == 0);

    /* eine kurze nachricht: genau eine zeile */
    CHECK(chat_append(&c, CHAT_ROLE_USER, "hallo") == 0);
    CHECK(chat_wrap(&c, 20, lines, 32) == 1);
    CHECK(lines[0].first);
    CHECK(lines[0].msg == 0);
    CHECK(lines[0].off == 0);
    CHECK(lines[0].len == 5);
    CHECK(lines[0].role == CHAT_ROLE_USER);

    /* word-wrap am leerzeichen: "aaa bbb" (7) + "ccc" */
    chat_clear(&c);
    CHECK(chat_append(&c, CHAT_ROLE_ASSISTANT, "aaa bbb ccc") == 0);
    CHECK(chat_wrap(&c, 8, lines, 32) == 2);
    CHECK(lines[0].first);
    CHECK(lines[0].off == 0);
    CHECK(lines[0].len == 7);
    CHECK(lines[0].role == CHAT_ROLE_ASSISTANT);
    CHECK(!lines[1].first); /* folgezeile: einrueckung statt label */
    CHECK(lines[1].off == 8);
    CHECK(lines[1].len == 3);

    /* ueberlanges wort: hart an der breite */
    chat_clear(&c);
    CHECK(chat_append(&c, CHAT_ROLE_ASSISTANT, "aaaaaaaa") == 0);
    CHECK(chat_wrap(&c, 5, lines, 32) == 2);
    CHECK(lines[0].len == 5);
    CHECK(lines[1].off == 5);
    CHECK(lines[1].len == 3);

    /* explizite umbrueche; trailing '\n' erzeugt KEINE leerzeile */
    chat_clear(&c);
    CHECK(chat_append(&c, CHAT_ROLE_USER, "a\nb\n") == 0);
    CHECK(chat_wrap(&c, 20, lines, 32) == 2);
    CHECK(lines[0].len == 1);
    CHECK(lines[0].first);
    CHECK(lines[1].off == 2);
    CHECK(lines[1].len == 1);
    CHECK(!lines[1].first);

    /* leere nachricht: genau eine leere label-zeile */
    chat_clear(&c);
    CHECK(chat_append(&c, CHAT_ROLE_ASSISTANT, "") == 0);
    CHECK(chat_wrap(&c, 20, lines, 32) == 1);
    CHECK(lines[0].first);
    CHECK(lines[0].len == 0);

    /* utf-8: ein codepoint = 1 zelle, laenge in BYTES */
    chat_clear(&c);
    CHECK(chat_append(&c, CHAT_ROLE_USER, "\xC3\xA4\xC3\xB6") == 0);
    CHECK(chat_wrap(&c, 2, lines, 32) == 1); /* 2 zellen passen */
    CHECK(lines[0].len == 4);                /* aber 4 bytes lang */

    /* utf-8-umbruch: niemals mitten im codepoint brechen */
    chat_clear(&c);
    CHECK(chat_append(&c, CHAT_ROLE_USER,
                      "\xC3\xA4\xC3\xA4 \xC3\xA4\xC3\xA4") == 0);
    CHECK(chat_wrap(&c, 2, lines, 32) == 2);
    CHECK(lines[0].len == 4); /* "\xC3\xA4\xC3\xA4" */
    CHECK(lines[1].off == 5);
    CHECK(lines[1].len == 4);

    /* CR wird schon beim anhaengen gefiltert */
    chat_clear(&c);
    CHECK(chat_append(&c, CHAT_ROLE_USER, "a\rb") == 0);
    CHECK(strcmp(c.msgs[0].text, "ab") == 0);
    CHECK(chat_wrap(&c, 20, lines, 32) == 1);
    CHECK(lines[0].len == 2);

    /* mehrere nachrichten: msg/role wandern mit */
    chat_clear(&c);
    CHECK(chat_append(&c, CHAT_ROLE_USER, "u") == 0);
    CHECK(chat_append(&c, CHAT_ROLE_ERROR, "e") == 0);
    CHECK(chat_wrap(&c, 20, lines, 32) == 2);
    CHECK(lines[0].role == CHAT_ROLE_USER);
    CHECK(lines[0].msg == 0);
    CHECK(lines[1].role == CHAT_ROLE_ERROR);
    CHECK(lines[1].msg == 1);

    /* arena zu klein: rueckgabe zaehlt trotzdem alles */
    chat_clear(&c);
    CHECK(chat_append(&c, CHAT_ROLE_USER, "a\nb\nc\nd\n") == 0);
    /* marker jenseits von out_max: chat_wrap darf ihn nicht anfassen.
     * ohne marker haengt die pruefung am zufaelligen stack-inhalt und
     * faellt nur im release-build auf. */
    lines[2].len = 4242;
    CHECK(chat_wrap(&c, 20, lines, 2) == 4);
    CHECK(lines[2].len == 4242); /* nicht mehr gefuellt */

    chat_free(&c);
}

static void test_layout(void)
{
    /* --- layout: scroll-normalisierung im viewport --- */
    AppState st = {0};
    input_init(&st.input);
    Config cfg = {0};

    /* verlauf GROESSER als das viewport (24 zeilen terminal:
     * input-box 3 zeilen -> chat_h = 20), damit scroll greift */
    for (int i = 1; i <= 30; i++) {
        char buf[32];
        snprintf(buf, sizeof buf, "zeile %d", i);
        CHECK(chat_append(&st.chat, CHAT_ROLE_USER, buf) == 0);
    }
    st.chat_scroll = 0; /* unten */

    Layout lt;
    layout_compute(&lt, 24, 80, MODE_INPUT, &st, &cfg);
    CHECK(lt.chat_h == lt.input_top - 1);
    CHECK((int)lt.chat_lines_len == 30);
    CHECK(lt.chat_h > 0);
    CHECK(st.chat_scroll == 0);
    CHECK(lt.chat_first == 30 - lt.chat_h); /* letzte chat_h zeilen */
    /* slot-mapping: viewport-zeile 1 = erste sichtbare verlaufs-zeile */
    Slot s1 = layout_slot(&lt, 1);
    CHECK(s1.kind == SLOT_MSG_USER);
    CHECK(s1.index == lt.chat_first);
    Slot sLast = layout_slot(&lt, lt.chat_h);
    CHECK(sLast.kind == SLOT_MSG_USER);
    CHECK(sLast.index == (int)lt.chat_lines_len - 1);

    /* zu weit gescrollt: layout klemmt und schreibt zurueck */
    st.chat_scroll = 999;
    layout_compute(&lt, 24, 80, MODE_INPUT, &st, &cfg);
    CHECK(st.chat_scroll == 30 - lt.chat_h);
    CHECK(lt.chat_first == 0);

    /* pgup um 2 zeilen: viewport rutscht mit */
    st.chat_scroll = 2;
    layout_compute(&lt, 24, 80, MODE_INPUT, &st, &cfg);
    CHECK(st.chat_scroll == 2);
    CHECK(lt.chat_first == 30 - 2 - lt.chat_h);
    CHECK(lt.chat_first > 0);

    /* verlauf KLEINER als viewport: oben anfangen, kein scroll */
    st.chat_scroll = 5;
    chat_clear(&st.chat);
    CHECK(chat_append(&st.chat, CHAT_ROLE_USER, "eins") == 0);
    CHECK(chat_append(&st.chat, CHAT_ROLE_ASSISTANT, "zwei") == 0);
    layout_compute(&lt, 24, 80, MODE_INPUT, &st, &cfg);
    CHECK(st.chat_scroll == 0);
    CHECK(lt.chat_first == 0);
    Slot sA = layout_slot(&lt, 1);
    CHECK(sA.kind == SLOT_MSG_USER);
    Slot sB = layout_slot(&lt, 2);
    CHECK(sB.kind == SLOT_MSG_ASSISTANT);
    Slot sEmpty = layout_slot(&lt, 3);
    CHECK(sEmpty.kind == SLOT_BLANK); /* nichts mehr im verlauf */

    /* zeilen unterm chat-fenster sind keine chat-zeilen: rahmen und
     * eingabefeld liegen unterhalb */
    Slot sIn = layout_slot(&lt, lt.input_top);
    CHECK(sIn.kind == SLOT_BORDER);
    Slot sLine = layout_slot(&lt, lt.input_top + 1);
    CHECK(sLine.kind == SLOT_INPUT);

    input_free(&st.input);
    chat_free(&st.chat);
}

int main(void)
{
    test_append_pop_clear();
    test_flatten();
    test_wrap();
    test_layout();
    return test_report();
}