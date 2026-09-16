#define _POSIX_C_SOURCE 200809L // NOLINT(bugprone-reserved-identifier)

#include <stdlib.h>
#include <string.h>

#include "chat.h"
#include "context.h"
#include "test.h"
#include "utils.h"

/* eine assistant-nachricht mit genau einem tool-call anhaengen */
static void append_call(Chat *chat, const char *name, const char *args)
{
    CHECK(chat_append(chat, CHAT_ROLE_ASSISTANT, "") == 0);
    ChatToolCall *calls = calloc(1, sizeof *calls);
    CHECK(calls != NULL);
    if (calls == NULL) {
        return;
    }
    calls[0].id = dup_str("call_1");
    calls[0].name = dup_str(name);
    calls[0].arguments = dup_str(args);
    CHECK(chat_set_tool_calls(chat, calls, 1) == 0);
}

static void test_tokens(void)
{
    /* --- schaetzung: aufrunden, NULL ist 0 --- */
    CHECK(ctx_tokens_text(NULL) == 0);
    CHECK(ctx_tokens_text("") == 0);
    CHECK(ctx_tokens_text("abcd") == 1);             /* genau ein token  */
    CHECK(ctx_tokens_text("abcde") == 2);            /* rest aufrunden   */
    CHECK(ctx_tokens_text("12345678") == 2);         /* 8 bytes          */
    CHECK(ctx_tokens_text("\xC3\xA4\xC3\xB6") == 1); /* utf-8: bytes  */

    /* --- nachrichten: rolle entscheidet, ob sie ueberhaupt zaehlt --- */
    Chat chat = {0};
    CHECK(chat_append(&chat, CHAT_ROLE_USER, "hallo welt") == 0);
    CHECK(chat_append(&chat, CHAT_ROLE_ERROR, "http 500: kaputt") == 0);
    CHECK(chat_append(&chat, CHAT_ROLE_NOTICE, "verlauf gekuerzt") == 0);

    CHECK(ctx_tokens_message(NULL) == 0);
    CHECK(ctx_tokens_message(&chat.msgs[0]) ==
          CTX_MSG_OVERHEAD + ctx_tokens_text("hallo welt"));
    /* lokale meldungen gehen nie raus und kosten darum nichts */
    CHECK(ctx_tokens_message(&chat.msgs[1]) == 0);
    CHECK(ctx_tokens_message(&chat.msgs[2]) == 0);

    /* tool-calls kosten extra: gleicher text, mehr tokens */
    size_t plain = ctx_tokens_message(&chat.msgs[0]);
    append_call(&chat, "bash", "{\"command\":\"ls -la /tmp\"}");
    CHECK(ctx_tokens_message(&chat.msgs[3]) > plain);
    CHECK(ctx_tokens_message(&chat.msgs[3]) >=
          CTX_MSG_OVERHEAD + CTX_CALL_OVERHEAD);

    /* tool-ergebnis: die call-id zaehlt mit */
    CHECK(chat_append_tool(&chat, "call_1", "ok") == 0);
    CHECK(ctx_tokens_message(&chat.msgs[4]) >
          CTX_MSG_OVERHEAD + ctx_tokens_text("ok"));

    /* --- tool-definitionen haengen an jeder anfrage --- */
    CHECK(ctx_tokens_tools() > 0);
    CHECK(ctx_tokens_tools() == ctx_tokens_tools()); /* deterministisch */

    /* --- request-schaetzung: tools + system + nachrichten ab from --- */
    size_t all = ctx_tokens_request(&chat, 0, NULL);
    CHECK(all > ctx_tokens_tools());
    CHECK(ctx_tokens_request(&chat, chat.len, NULL) == ctx_tokens_tools());
    CHECK(ctx_tokens_request(&chat, 0, "du bist ein test-agent") > all);
    CHECK(ctx_tokens_request(NULL, 0, NULL) == ctx_tokens_tools());
    /* from waechst -> schaetzung faellt monoton */
    CHECK(ctx_tokens_request(&chat, 1, NULL) <= all);

    chat_free(&chat);
}

static void test_budget(void)
{
    /* --- kein fenster bekannt: nicht kuerzen --- */
    CHECK(ctx_budget(NULL, NULL, NULL) == CTX_NO_LIMIT);
    Model unknown = {0};
    CHECK(ctx_budget(&unknown, NULL, NULL) == CTX_NO_LIMIT);

    /* --- normales fenster: platz fuer antwort und tools geht ab --- */
    Model big = {0};
    big.context_window = 128000;
    size_t budget = ctx_budget(&big, NULL, NULL);
    CHECK(budget != CTX_NO_LIMIT);
    CHECK(budget > 0);
    CHECK(budget < big.context_window);
    CHECK(budget + ctx_tokens_tools() < big.context_window);

    /* der system-prompt geht ebenfalls ab */
    size_t with_sys = ctx_budget(&big, "du bist ein sehr langer prompt", NULL);
    CHECK(with_sys < budget);

    /* --- winziges fenster: nichts uebrig, aber kein unterlauf --- */
    Model tiny = {0};
    tiny.context_window = 64;
    CHECK(ctx_budget(&tiny, NULL, NULL) == 0);

    /* --- kalibrierung schrumpft das budget, wenn wir unterschaetzen --- */
    CtxUsage low = {.scale = CTX_SCALE_ONE / 2};  /* wir ueberschaetzen */
    CtxUsage high = {.scale = CTX_SCALE_ONE * 2}; /* wir unterschaetzen */
    CHECK(ctx_budget(&big, NULL, &low) > budget);
    CHECK(ctx_budget(&big, NULL, &high) < budget);

    /* ungeeicht (scale 0) aendert nichts */
    CtxUsage fresh = {0};
    CHECK(ctx_budget(&big, NULL, &fresh) == budget);
}

static void test_trim(void)
{
    /* --- randfaelle --- */
    CHECK(ctx_trim_start(NULL, 100) == 0);
    Chat empty = {0};
    CHECK(ctx_trim_start(&empty, 100) == 0);

    Chat chat = {0};
    CHECK(chat_append(&chat, CHAT_ROLE_USER, "erste frage") == 0);
    CHECK(chat_append(&chat, CHAT_ROLE_ASSISTANT, "erste antwort") == 0);
    CHECK(chat_append(&chat, CHAT_ROLE_USER, "zweite frage") == 0);

    /* kein limit / grosses budget: alles geht mit */
    CHECK(ctx_trim_start(&chat, CTX_NO_LIMIT) == 0);
    CHECK(ctx_trim_start(&chat, 100000) == 0);

    /* budget 0: nur die letzte nachricht ueberlebt */
    CHECK(ctx_trim_start(&chat, 0) == 2);

    /* budget fuer genau die letzten beiden */
    size_t two =
        ctx_tokens_message(&chat.msgs[1]) + ctx_tokens_message(&chat.msgs[2]);
    CHECK(ctx_trim_start(&chat, two) == 1);
    CHECK(ctx_trim_start(&chat, two - 1) == 2);

    chat_free(&chat);
}

static void test_trim_tool_groups(void)
{
    /* verlauf mit einer kompletten tool-runde in der mitte:
     *   [0] user, [1] assistant+call, [2] tool, [3] user */
    Chat chat = {0};
    CHECK(chat_append(&chat, CHAT_ROLE_USER, "alte frage mit viel text") == 0);
    append_call(&chat, "bash", "{\"command\":\"ls\"}");
    CHECK(chat_append_tool(&chat, "call_1", "datei-a datei-b") == 0);
    CHECK(chat_append(&chat, CHAT_ROLE_USER, "neue frage") == 0);
    CHECK(chat.len == 4);

    /* budget reicht fuer [2]+[3], aber [2] ist ein tool-ergebnis
     * ohne seinen call: die angeschnittene gruppe faellt ganz weg */
    size_t tail =
        ctx_tokens_message(&chat.msgs[2]) + ctx_tokens_message(&chat.msgs[3]);
    CHECK(ctx_trim_start(&chat, tail) == 3);

    /* reicht es auch fuer den call davor, bleibt die gruppe ganz */
    size_t group = tail + ctx_tokens_message(&chat.msgs[1]);
    CHECK(ctx_trim_start(&chat, group) == 1);

    /* gross genug fuer alles */
    CHECK(ctx_trim_start(&chat, group + ctx_tokens_message(&chat.msgs[0])) ==
          0);
    chat_free(&chat);
}

static void test_trim_unfinished_round(void)
{
    /* mitten im agent-loop endet der verlauf mit tool-ergebnissen.
     * die duerfen nie ohne ihren assistant-call gesendet werden –
     * auch dann nicht, wenn das budget 0 ist. */
    Chat chat = {0};
    CHECK(chat_append(&chat, CHAT_ROLE_USER, "tu was") == 0);
    append_call(&chat, "bash", "{\"command\":\"echo hi\"}");
    CHECK(chat_append_tool(&chat, "call_1", "hi") == 0);

    CHECK(ctx_trim_start(&chat, 0) == 1); /* assistant+tool bleiben */
    CHECK(ctx_trim_start(&chat, 1) == 1);
    CHECK(ctx_trim_start(&chat, 100000) == 0);
    chat_free(&chat);
}

static void test_trim_local_only(void)
{
    /* nur lokale meldungen: es gibt nichts zu senden */
    Chat chat = {0};
    CHECK(chat_append(&chat, CHAT_ROLE_ERROR, "http 500") == 0);
    CHECK(chat_append(&chat, CHAT_ROLE_NOTICE, "verlauf gekuerzt") == 0);
    CHECK(ctx_trim_start(&chat, 0) == 0);
    CHECK(ctx_trim_start(&chat, 100000) == 0);
    chat_free(&chat);

    /* lokale meldungen am ende verschieben die untergrenze nicht:
     * gezaehlt wird ab der letzten ECHTEN nachricht */
    Chat mixed = {0};
    CHECK(chat_append(&mixed, CHAT_ROLE_USER, "frage") == 0);
    CHECK(chat_append(&mixed, CHAT_ROLE_ASSISTANT, "antwort") == 0);
    CHECK(chat_append(&mixed, CHAT_ROLE_NOTICE, "hinweis") == 0);
    CHECK(ctx_trim_start(&mixed, 0) == 1);
    chat_free(&mixed);
}

static void test_calibrate(void)
{
    ctx_calibrate(NULL, 100, 100); /* darf nicht knallen */

    CtxUsage u = {0};
    CHECK(u.scale == 0); /* ungeeicht */

    /* ohne zahl von der api bleibt der faktor, wie er war */
    ctx_calibrate(&u, 100, 0);
    CHECK(u.scale == 0);
    CHECK(u.estimated == 100); /* die schaetzung wird trotzdem notiert */
    ctx_calibrate(&u, 0, 500);
    CHECK(u.scale == 0);

    /* punktgenau geschaetzt -> faktor 1000 */
    ctx_calibrate(&u, 1000, 1000);
    CHECK(u.scale == CTX_SCALE_ONE);
    CHECK(u.prompt_tokens == 1000);

    /* um die haelfte unterschaetzt -> faktor 2000 */
    ctx_calibrate(&u, 1000, 2000);
    CHECK(u.scale == 2 * CTX_SCALE_ONE);

    /* ausreisser werden geklemmt (sonst kippt das budget) */
    ctx_calibrate(&u, 1, 100000);
    CHECK(u.scale > CTX_SCALE_ONE && u.scale <= 4000);
    ctx_calibrate(&u, 100000, 1);
    CHECK(u.scale >= 250 && u.scale < CTX_SCALE_ONE);
}

/* verbrauchs-buchhaltung fuer die statuszeile: getrennt von der
 * eichung, summiert ueber die ganze sitzung. */
static void test_account(void)
{
    ctx_account(NULL, 10, 5); /* darf nicht knallen */

    CtxUsage u = {0};
    CHECK(u.total_prompt == 0 && u.total_completion == 0);

    ctx_account(&u, 100, 20);
    CHECK(u.total_prompt == 100);
    CHECK(u.total_completion == 20);

    /* mehrere runden summieren sich */
    ctx_account(&u, 250, 30);
    CHECK(u.total_prompt == 350);
    CHECK(u.total_completion == 50);

    /* fehlende zahlen (api hat nichts geliefert) aendern nichts */
    ctx_account(&u, 0, 0);
    ctx_account(&u, -5, -1);
    CHECK(u.total_prompt == 350);
    CHECK(u.total_completion == 50);

    /* einzeln zaehlen ist erlaubt: nur prompt, nur completion */
    ctx_account(&u, 10, 0);
    CHECK(u.total_prompt == 360 && u.total_completion == 50);
    ctx_account(&u, 0, 7);
    CHECK(u.total_prompt == 360 && u.total_completion == 57);

    /* die buchhaltung fasst den korrekturfaktor nicht an */
    CHECK(u.scale == 0);
}

int main(void)
{
    test_tokens();
    test_budget();
    test_trim();
    test_trim_tool_groups();
    test_trim_unfinished_round();
    test_trim_local_only();
    test_calibrate();
    test_account();
    return test_report();
}
