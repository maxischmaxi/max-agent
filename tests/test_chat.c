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
    /* am ende des verlaufs: nichts unten verdeckt, aber oben – der
     * "weiter oben"-hinweis belegt zeile 1 und kostet eine
     * verlaufs-zeile */
    CHECK(lt.more_below == 0);
    CHECK(lt.more_below_row == 0);
    CHECK(lt.more_above_row == 1);
    CHECK(lt.chat_top == 2);
    int view = lt.chat_h - 1; /* eine zeile geht an den hinweis */
    CHECK(lt.chat_first == 30 - view);
    CHECK(lt.more_above == lt.chat_first);
    /* slot-mapping: die erste verlaufs-zeile liegt jetzt in zeile 2 */
    Slot sHint = layout_slot(&lt, 1);
    CHECK(sHint.kind == SLOT_MORE_ABOVE);
    Slot s1 = layout_slot(&lt, lt.chat_top);
    CHECK(s1.kind == SLOT_MSG_USER);
    CHECK(s1.index == lt.chat_first);
    Slot sLast = layout_slot(&lt, lt.chat_h);
    CHECK(sLast.kind == SLOT_MSG_USER);
    CHECK(sLast.index == (int)lt.chat_lines_len - 1);

    /* zu weit gescrollt: layout klemmt und schreibt zurueck. ganz
     * oben faellt der obere hinweis weg, dafuer kommt der untere */
    st.chat_scroll = 999;
    layout_compute(&lt, 24, 80, MODE_INPUT, &st, &cfg);
    CHECK(lt.chat_first == 0);
    CHECK(lt.more_above == 0);
    CHECK(lt.more_above_row == 0);
    CHECK(lt.chat_top == 1);
    CHECK(lt.more_below > 0);
    CHECK(lt.more_below_row == lt.input_top - 1);
    CHECK(st.chat_scroll == 30 - (lt.chat_h - 1));
    CHECK(layout_slot(&lt, lt.more_below_row).kind == SLOT_MORE_BELOW);

    /* pgup um 2 zeilen: viewport rutscht mit, jetzt sind BEIDE
     * hinweise noetig und kosten zusammen zwei zeilen */
    st.chat_scroll = 2;
    layout_compute(&lt, 24, 80, MODE_INPUT, &st, &cfg);
    CHECK(st.chat_scroll == 2);
    CHECK(lt.more_below == 2);
    CHECK(lt.more_above > 0);
    CHECK(lt.chat_first == 30 - 2 - (lt.chat_h - 2));
    CHECK(lt.chat_first > 0);
    CHECK(layout_slot(&lt, 1).kind == SLOT_MORE_ABOVE);
    CHECK(layout_slot(&lt, lt.input_top - 1).kind == SLOT_MORE_BELOW);

    /* verlauf KLEINER als viewport: oben anfangen, kein scroll */
    st.chat_scroll = 5;
    chat_clear(&st.chat);
    CHECK(chat_append(&st.chat, CHAT_ROLE_USER, "eins") == 0);
    CHECK(chat_append(&st.chat, CHAT_ROLE_ASSISTANT, "zwei") == 0);
    layout_compute(&lt, 24, 80, MODE_INPUT, &st, &cfg);
    CHECK(st.chat_scroll == 0);
    CHECK(lt.chat_first == 0);
    /* alles sichtbar: kein hinweis, verlauf beginnt wieder bei 1 */
    CHECK(lt.more_above == 0);
    CHECK(lt.more_below == 0);
    CHECK(lt.chat_top == 1);
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

/* die scroll-hinweise an ihren raendern: sie kosten platz, also
 * muessen sie verschwinden, wenn keiner da ist – und dem thinking-
 * indikator ausweichen, wenn gerade eine anfrage laeuft. */
static void test_scroll_hints(void)
{
    AppState st = {0};
    input_init(&st.input);
    Config cfg = {0};
    Layout lt;

    for (int i = 1; i <= 40; i++) {
        char buf[32];
        snprintf(buf, sizeof buf, "zeile %d", i);
        CHECK(chat_append(&st.chat, CHAT_ROLE_USER, buf) == 0);
    }

    /* --- waehrend einer anfrage: der untere hinweis rueckt ueber
     *     den thinking-indikator --- */
    st.chat_scroll = 3;
    st.busy = true;
    layout_compute(&lt, 24, 80, MODE_INPUT, &st, &cfg);
    CHECK(lt.busy_row == lt.input_top - 1);
    CHECK(lt.more_below == 3);
    CHECK(lt.more_below_row == lt.busy_row - 1);
    CHECK(layout_slot(&lt, lt.busy_row).kind == SLOT_BUSY);
    CHECK(layout_slot(&lt, lt.more_below_row).kind == SLOT_MORE_BELOW);
    /* der verlauf liegt dazwischen, ohne ueberschneidung */
    CHECK(layout_slot(&lt, lt.chat_top).kind == SLOT_MSG_USER);
    CHECK(layout_slot(&lt, lt.more_below_row - 1).kind == SLOT_MSG_USER);
    st.busy = false;

    /* --- winziges terminal: lieber verlauf zeigen als hinweise --- */
    for (int rows = 5; rows <= 8; rows++) {
        st.chat_scroll = 3;
        layout_compute(&lt, rows, 80, MODE_INPUT, &st, &cfg);
        int hints =
            (lt.more_above_row > 0 ? 1 : 0) + (lt.more_below_row > 0 ? 1 : 0);
        /* nie mehr hinweise als platz da ist, und immer mindestens
         * eine zeile echter verlauf */
        CHECK(hints <= lt.chat_h - 1 || hints == 0);
        if (lt.chat_h > 0) {
            CHECK(lt.chat_top <= lt.chat_h);
        }
        /* die hinweis-zeilen ueberschneiden sich nie */
        if (lt.more_above_row > 0 && lt.more_below_row > 0) {
            CHECK(lt.more_above_row < lt.more_below_row);
        }
    }

    /* --- der zurueckgeschriebene scroll passt zum layout --- */
    st.chat_scroll = 0;
    layout_compute(&lt, 24, 80, MODE_INPUT, &st, &cfg);
    CHECK(st.chat_scroll == 0);
    CHECK(lt.more_below == 0);
    /* summe stimmt: oben verdeckt + sichtbar + unten verdeckt */
    int visible = 0;
    for (int row = 1; row < lt.input_top; row++) {
        Slot s = layout_slot(&lt, row);
        if (s.kind == SLOT_MSG_USER || s.kind == SLOT_MSG_ASSISTANT) {
            visible++;
        }
    }
    CHECK(lt.more_above + visible + lt.more_below == (int)lt.chat_lines_len);

    input_free(&st.input);
    chat_free(&st.chat);
}

/* soft-wrap im eingabefeld: die box waechst mit den umgebrochenen
 * zeilen, ein resize rechnet neu – und der TEXT bleibt dabei
 * unangetastet. das ist der grund, warum der umbruch nicht ins
 * datenmodell geht. */
static void test_input_wrap_layout(void)
{
    AppState st = {0};
    input_init(&st.input);
    Config cfg = {0};
    Layout lt;

    /* --- kurzer text: eine zeile, box wie gehabt --- */
    input_set_text(&st.input, "kurz");
    layout_compute(&lt, 24, 80, MODE_INPUT, &st, &cfg);
    CHECK(lt.input_bottom - lt.input_top == 2); /* 1 zeile + 2 rahmen */
    CHECK(lt.input_first == 0);
    CHECK(lt.input_w > 0);
    CHECK(layout_slot(&lt, lt.input_top + 1).kind == SLOT_INPUT);

    /* --- text laenger als das feld: die box waechst --- */
    size_t n = (size_t)lt.input_w * 3;
    char *lang = malloc(n + 1);
    CHECK(lang != NULL);
    if (lang == NULL) {
        return;
    }
    memset(lang, 'x', n);
    lang[n] = '\0';
    input_set_text(&st.input, lang);
    CHECK(st.input.count == 1); /* EINE logische zeile */

    layout_compute(&lt, 24, 80, MODE_INPUT, &st, &cfg);
    CHECK(lt.input_bottom - lt.input_top == 4);  /* 3 zeilen + rahmen */
    CHECK(strcmp(st.input.lines[0], lang) == 0); /* text unveraendert */

    /* --- schmaleres fenster: mehr zeilen, gleicher text --- */
    layout_compute(&lt, 24, 40, MODE_INPUT, &st, &cfg);
    int narrow = lt.input_bottom - lt.input_top - 1;
    CHECK(narrow > 3);
    CHECK(strcmp(st.input.lines[0], lang) == 0);
    CHECK(st.input.count == 1);

    /* --- breiteres fenster: wieder weniger zeilen --- */
    layout_compute(&lt, 24, 200, MODE_INPUT, &st, &cfg);
    int wide = lt.input_bottom - lt.input_top - 1;
    CHECK(wide < narrow);
    CHECK(strcmp(st.input.lines[0], lang) == 0);
    CHECK(st.input.count == 1);
    free(lang);

    /* --- sehr viel text: die box deckelt und scrollt mit --- */
    char *riesig = malloc(4001);
    CHECK(riesig != NULL);
    if (riesig == NULL) {
        return;
    }
    memset(riesig, 'y', 4000);
    riesig[4000] = '\0';
    input_set_text(&st.input, riesig);
    free(riesig);
    layout_compute(&lt, 24, 80, MODE_INPUT, &st, &cfg);
    int box_h = lt.input_bottom - lt.input_top - 1;
    CHECK(box_h > 0);
    CHECK(lt.input_top >= 1);  /* nie ueber den rand hinaus */
    CHECK(lt.input_top >= 2);  /* zwei zeilen bleiben oben frei */
    CHECK(lt.input_first > 0); /* es wird gescrollt */
    /* die cursor-zeile ist sichtbar */
    size_t crow = 0;
    input_cursor_screen(&st.input, lt.input_w, &crow, NULL);
    CHECK(crow >= lt.input_first);
    CHECK(crow < lt.input_first + (size_t)box_h);

    /* cursor nach vorn: das feld scrollt zurueck */
    input_cursor_line_set(&st.input, 0);
    input_cursor_set(&st.input, 0);
    layout_compute(&lt, 24, 80, MODE_INPUT, &st, &cfg);
    CHECK(lt.input_first == 0);

    /* --- explizite newlines bleiben eigene zeilen --- */
    input_set_text(&st.input, "a\nb\nc");
    CHECK(st.input.count == 3);
    layout_compute(&lt, 24, 80, MODE_INPUT, &st, &cfg);
    CHECK(lt.input_bottom - lt.input_top == 4); /* 3 zeilen + rahmen */

    input_free(&st.input);
    chat_free(&st.chat);
}

/* die zwei statuszeilen unten: sie sind fest reserviert, gehoeren
 * niemandem sonst, und alles andere rueckt darueber. */
static void test_status_rows(void)
{
    AppState st = {0};
    input_init(&st.input);
    Config cfg = {0};
    Layout lt;

    layout_compute(&lt, 24, 80, MODE_INPUT, &st, &cfg);
    /* zeile 24 bleibt frei (dort parkt frame_end den cursor),
     * darueber die beiden statuszeilen */
    CHECK(lt.status_row == 24 - STATUS_H);
    CHECK(layout_slot(&lt, lt.status_row).kind == SLOT_STATUS_MODEL);
    CHECK(layout_slot(&lt, lt.status_row + 1).kind == SLOT_STATUS_TOKENS);

    /* das eingabefeld liegt komplett DARUEBER */
    CHECK(lt.input_bottom < lt.status_row);
    CHECK(lt.input_top < lt.input_bottom);

    /* keine andere zeile beansprucht die statuszeilen */
    for (int row = 1; row < lt.status_row; row++) {
        Slot s = layout_slot(&lt, row);
        CHECK(s.kind != SLOT_STATUS_MODEL);
        CHECK(s.kind != SLOT_STATUS_TOKENS);
    }

    /* --- mit befehlsliste: alles rueckt hoch, status bleibt --- */
    input_set_text(&st.input, "/");
    st.cmd_active = true;
    layout_compute(&lt, 24, 80, MODE_INPUT, &st, &cfg);
    CHECK(lt.status_row == 24 - STATUS_H);
    CHECK(lt.cmd_h > 0);
    CHECK(lt.cmd_top + lt.cmd_h <= lt.status_row); /* kein ueberlapp */
    CHECK(layout_slot(&lt, lt.status_row).kind == SLOT_STATUS_MODEL);
    st.cmd_active = false;
    input_reset(&st.input);

    /* --- mit quit-meldung: die liegt ueber dem status --- */
    st.confirm_quit = true;
    layout_compute(&lt, 24, 80, MODE_INPUT, &st, &cfg);
    CHECK(lt.quit_row > 0);
    CHECK(lt.quit_row < lt.status_row);
    CHECK(layout_slot(&lt, lt.quit_row).kind == SLOT_QUIT);
    CHECK(layout_slot(&lt, lt.status_row).kind == SLOT_STATUS_MODEL);
    st.confirm_quit = false;

    /* --- in den dialogen ebenfalls immer sichtbar --- */
    st.models_dialog = true;
    layout_compute(&lt, 24, 80, MODE_MODELS, &st, &cfg);
    CHECK(layout_slot(&lt, lt.status_row).kind == SLOT_STATUS_MODEL);
    CHECK(layout_slot(&lt, lt.status_row + 1).kind == SLOT_STATUS_TOKENS);
    CHECK(lt.box_bottom < lt.status_row); /* dialog bleibt darueber */
    st.models_dialog = false;

    /* --- kleine terminals: entweder passt der status sauber
     *     darunter, oder er faellt ganz weg. nie ueberlappen. --- */
    for (int rows = 1; rows <= 12; rows++) {
        layout_compute(&lt, rows, 80, MODE_INPUT, &st, &cfg);
        if (lt.status_row == 0) {
            continue; /* kein platz: bewusst kein status */
        }
        CHECK(lt.status_row >= 1);
        CHECK(lt.status_row + STATUS_H - 1 <= rows);
        CHECK(lt.input_bottom < lt.status_row); /* box liegt darueber */
        CHECK(lt.input_top >= 1);
        CHECK(layout_slot(&lt, lt.status_row).kind == SLOT_STATUS_MODEL);
        CHECK(layout_slot(&lt, lt.status_row + 1).kind == SLOT_STATUS_TOKENS);
    }

    input_free(&st.input);
    chat_free(&st.chat);
}

int main(void)
{
    test_append_pop_clear();
    test_flatten();
    test_wrap();
    test_layout();
    test_scroll_hints();
    test_input_wrap_layout();
    test_status_rows();
    return test_report();
}