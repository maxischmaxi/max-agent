#include "context.h"

#include <stdlib.h>
#include <string.h>

#include "tools.h"

/* platz, der im fenster fuer die ANTWORT frei bleiben muss. ein
 * fester wert waere fuer kleine fenster zu gross und fuer grosse zu
 * knapp, darum ein achtel – geklemmt, damit beides praktikabel
 * bleibt. */
#define CTX_RESERVE_DIV 8
#define CTX_RESERVE_MIN 512
#define CTX_RESERVE_MAX 8192

/* grenzen des korrekturfaktors: eine kaputte usage-zahl (oder ein
 * provider, der prompt_tokens anders definiert) soll das budget
 * weder auf null druecken noch das fenster sprengen */
#define CTX_SCALE_MIN 250
#define CTX_SCALE_MAX 4000

size_t ctx_tokens_text(const char *text)
{
    if (text == NULL) {
        return 0;
    }
    /* aufrunden: auch ein einzelnes zeichen ist ein token */
    return (strlen(text) + CTX_BYTES_PER_TOKEN - 1) / CTX_BYTES_PER_TOKEN;
}

size_t ctx_tokens_message(const ChatMessage *msg)
{
    if (msg == NULL || !chat_role_sent(msg->role)) {
        return 0; /* lokale meldung: kostet nichts, geht nie raus */
    }
    size_t n = CTX_MSG_OVERHEAD + ctx_tokens_text(msg->text);
    for (size_t i = 0; i < msg->tool_calls_len; i++) {
        n += CTX_CALL_OVERHEAD + ctx_tokens_text(msg->tool_calls[i].name) +
             ctx_tokens_text(msg->tool_calls[i].arguments);
    }
    n += ctx_tokens_text(msg->tool_call_id);
    return n;
}

size_t ctx_tokens_tools(void)
{
    size_t len = 0;
    const OaiTool *tools = tool_registry(&len);
    size_t n = 0;
    for (size_t i = 0; i < len; i++) {
        n += CTX_CALL_OVERHEAD + ctx_tokens_text(tools[i].function.name) +
             ctx_tokens_text(tools[i].function.description) +
             ctx_tokens_text(tools[i].function.parameters_json);
    }
    return n;
}

size_t ctx_tokens_request(const Chat *chat, size_t from,
                          const char *system_prompt, const char *summary)
{
    size_t n = ctx_tokens_tools();
    if (system_prompt != NULL && system_prompt[0] != '\0') {
        n += CTX_MSG_OVERHEAD + ctx_tokens_text(system_prompt);
    }
    n += ctx_summary_tokens(summary);
    if (chat == NULL) {
        return n;
    }
    for (size_t i = from; i < chat->len; i++) {
        n += ctx_tokens_message(&chat->msgs[i]);
    }
    return n;
}

/* praefix und suffix, mit denen die summary als user-nachricht in
 * jeden request wandert (pi-agenten-konvention) */
#define SUMMARY_PREFIX                                               \
    "The conversation history before this point was compacted into " \
    "the following summary:\n\n<summary>\n"
#define SUMMARY_SUFFIX "\n</summary>"

char *ctx_summary_wrap(const char *summary)
{
    if (summary == NULL || summary[0] == '\0') {
        return NULL;
    }
    size_t plen = strlen(SUMMARY_PREFIX);
    size_t slen = strlen(SUMMARY_SUFFIX);
    size_t len = strlen(summary);
    char *out = malloc(plen + len + slen + 1);
    if (out == NULL) {
        return NULL;
    }
    /* das letzte memcpy kopiert slen + 1 bytes – auch das '\0';
     * der analyzer sieht die terminierung nicht (false positive) */
    // NOLINTBEGIN(bugprone-not-null-terminated-result)
    memcpy(out, SUMMARY_PREFIX, plen);
    memcpy(out + plen, summary, len);
    memcpy(out + plen + len, SUMMARY_SUFFIX, slen + 1);
    // NOLINTEND(bugprone-not-null-terminated-result)
    return out;
}

size_t ctx_summary_tokens(const char *summary)
{
    if (summary == NULL || summary[0] == '\0') {
        return 0;
    }
    return CTX_MSG_OVERHEAD + ctx_tokens_text(SUMMARY_PREFIX) +
           ctx_tokens_text(summary) + ctx_tokens_text(SUMMARY_SUFFIX);
}

void ctx_reset(CtxUsage *usage)
{
    if (usage == NULL) {
        return;
    }
    free(usage->summary);
    usage->summary = NULL;
    usage->covered = 0;
    usage->compact_failed = false;
}

size_t ctx_budget(const Model *model, const char *system_prompt,
                  const char *summary, const CtxUsage *usage)
{
    if (model == NULL || model->context_window == 0) {
        return CTX_NO_LIMIT; /* fenster unbekannt: nicht kuerzen */
    }
    size_t window = model->context_window;

    size_t reserve = window / CTX_RESERVE_DIV;
    if (reserve < CTX_RESERVE_MIN) {
        reserve = CTX_RESERVE_MIN;
    }
    if (reserve > CTX_RESERVE_MAX) {
        reserve = CTX_RESERVE_MAX;
    }

    size_t fixed = reserve + ctx_tokens_tools();
    if (system_prompt != NULL && system_prompt[0] != '\0') {
        fixed += CTX_MSG_OVERHEAD + ctx_tokens_text(system_prompt);
    }
    fixed += ctx_summary_tokens(summary);
    if (fixed >= window) {
        return 0; /* fenster zu klein: nur die letzte gruppe passt */
    }
    size_t budget = window - fixed;

    /* gemessene abweichung einrechnen: unterschaetzt die heuristik
     * (scale > 1000), muss das budget entsprechend schrumpfen */
    if (usage != NULL && usage->scale > 0) {
        budget = (budget * CTX_SCALE_ONE) / (size_t)usage->scale;
    }
    return budget;
}

/* anfang der gruppe, zu der idx gehoert: ein tool-ergebnis gehoert
 * zum assistant-call davor und darf nie von ihm getrennt werden.
 * lokale meldungen (ERROR/NOTICE) koennen dazwischenstehen und
 * werden ueberlesen. */
static size_t group_start(const Chat *chat, size_t idx)
{
    while (idx > 0) {
        ChatRole role = chat->msgs[idx].role;
        if (role != CHAT_ROLE_TOOL && chat_role_sent(role)) {
            break; /* eigenstaendige nachricht: hier faengt sie an */
        }
        idx--;
    }
    return idx;
}

size_t ctx_trim_start(const Chat *chat, size_t budget)
{
    if (chat == NULL || chat->len == 0 || budget == CTX_NO_LIMIT) {
        return 0;
    }

    /* letzte nachricht, die ueberhaupt rausgeht – samt ihrer
     * gruppe. sie ist die untergrenze: darunter wuerde der request
     * leer oder ungueltig. */
    size_t last = chat->len;
    for (size_t i = chat->len; i > 0; i--) {
        if (chat_role_sent(chat->msgs[i - 1].role)) {
            last = i - 1;
            break;
        }
    }
    if (last == chat->len) {
        return 0; /* nichts sendbares im verlauf */
    }
    size_t floor_idx = group_start(chat, last);

    /* von der untergrenze aus nach vorn sammeln, solange das
     * budget reicht */
    size_t start = floor_idx;
    size_t used = 0;
    for (size_t i = floor_idx; i < chat->len; i++) {
        used += ctx_tokens_message(&chat->msgs[i]);
    }
    for (size_t i = floor_idx; i > 0; i--) {
        size_t cost = ctx_tokens_message(&chat->msgs[i - 1]);
        if (used + cost > budget) {
            break;
        }
        used += cost;
        start = i - 1;
    }

    /* angeschnittene gruppe ganz fallen lassen: fuehrende tool-
     * ergebnisse haben ihren assistant-call verloren */
    while (start < floor_idx && (chat->msgs[start].role == CHAT_ROLE_TOOL ||
                                 !chat_role_sent(chat->msgs[start].role))) {
        start++;
    }
    return start;
}

size_t ctx_cut_point(const Chat *chat, size_t floor, size_t keep_tokens)
{
    if (chat == NULL || chat->len == 0) {
        return 0;
    }
    /* letzte sendbare nachricht samt gruppen-anfang: sie bleibt
     * IMMER unverkuertzt draussen – ohne sie waere der request
     * leer oder ungueltig (wie der trim-floor) */
    size_t last = chat->len;
    for (size_t i = chat->len; i > 0; i--) {
        if (chat_role_sent(chat->msgs[i - 1].role)) {
            last = i - 1;
            break;
        }
    }
    if (last == chat->len) {
        return 0; /* nichts sendbares: keine summary noetig */
    }
    size_t hard_max = group_start(chat, last);
    /* floor (was ctx_trim_start sowieso fallen laesst) gehoert immer
     * zur summary-seite – aber nie auf kosten der pflicht-gruppe */
    size_t min_cut = (floor < hard_max) ? floor : hard_max;

    /* von hinten sammeln, bis keep_tokens erschoepft ist */
    size_t cut = chat->len;
    size_t used = 0;
    for (size_t i = chat->len; i > min_cut; i--) {
        size_t cost = ctx_tokens_message(&chat->msgs[i - 1]);
        if (used + cost > keep_tokens) {
            break;
        }
        used += cost;
        cut = i - 1;
    }
    if (cut > hard_max) {
        cut = hard_max; /* nichts passte: mindestens die pflicht-gruppe */
    }
    /* gruppen-ausrichtung: ein tool-ergebnis am fenster-anfang haette
     * seinen assistant-call verloren (ungueltiger request) – solche
     * halben gruppen rutschen komplett auf die summary-seite */
    while (cut < chat->len && (chat->msgs[cut].role == CHAT_ROLE_TOOL ||
                               !chat_role_sent(chat->msgs[cut].role))) {
        cut++;
    }
    return cut;
}

void ctx_calibrate(CtxUsage *usage, size_t estimated, int prompt_tokens)
{
    if (usage == NULL) {
        return;
    }
    usage->estimated = estimated;
    if (prompt_tokens <= 0 || estimated == 0) {
        return; /* keine zahl von der api: faktor bleibt, wie er war */
    }
    usage->prompt_tokens = prompt_tokens;

    size_t scale = ((size_t)prompt_tokens * CTX_SCALE_ONE) / estimated;
    if (scale < CTX_SCALE_MIN) {
        scale = CTX_SCALE_MIN;
    }
    if (scale > CTX_SCALE_MAX) {
        scale = CTX_SCALE_MAX;
    }
    usage->scale = (int)scale;
}

void ctx_account(CtxUsage *usage, int prompt_tokens, int completion_tokens)
{
    if (usage == NULL) {
        return;
    }
    if (prompt_tokens > 0) {
        usage->total_prompt += (size_t)prompt_tokens;
    }
    if (completion_tokens > 0) {
        usage->total_completion += (size_t)completion_tokens;
    }
}
