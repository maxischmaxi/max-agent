/* ------------------------------------------------------------------ */
/* openai_completions: client fuer die OpenAI chat-completions-API     */
/* (und jeden kompatiblen endpoint: Ollama, vLLM, llama.cpp-server,    */
/* LM Studio, OpenRouter, ...).                                        */
/*                                                                     */
/* 1:1-nachbau des npm "openai"-packages auf C mit libcurl + cJSON.    */
/* entspricht in der JS-version:                                       */
/*                                                                     */
/*   const client = new OpenAI({ apiKey, baseURL });                   */
/*   const completion = await client.chat.completions.create({         */
/*     model: 'gpt-4o-mini',                                           */
/*     messages: [{ role: 'user', content: '...' }],                   */
/*   });                                                               */
/*   console.log(completion.choices[0].message.content);               */
/*                                                                     */
/* alle optionen (temperature, top_p, max_tokens, stop, tools, ...)    */
/* sind wie im npm-package benannt; optionale werte bekommen ein       */
/* has_*-flag, das dem "feld nicht angegeben" in JS entspricht.        */
/* fehler werden nicht geworfen, sondern in OaiCompletionResult        */
/* zurueckgegeben (entspricht APIError / APIConnectionError).          */
/*                                                                     */
/* alle strings in den ergebnis-strukturen sind heap-kopien und        */
/* muessen ueber die *_free-funktionen freigegeben werden.             */
/* callbacks beim streaming duerfen die uebergebenen chunk-pointer     */
/* nur waehrend des aufrufs behalten; zum behalten selbst kopieren.    */
/*                                                                     */
/* beispiel 1: einfache anfrage (nicht-streaming)                      */
/*                                                                     */
/*   #include "openai_completions.h"                                   */
/*                                                                     */
/*   OaiClient client;                                                 */
/*   OaiClientOptions opts = {.api_key = getenv("OPENAI_API_KEY")};    */
/*   if (oai_client_init(&client, &opts) != 0) {                       */
/*       die("client-init fehlgeschlagen");                            */
/*   }                                                                 */
/*                                                                     */
/*   OaiMessage messages[] = {                                         */
/*       {.role = OAI_ROLE_SYSTEM,                                     */
/*        .content = "du bist ein hilfreicher assistent."},            */
/*       {.role = OAI_ROLE_USER, .content = "erklaere malloc."},       */
/*   };                                                                */
/*   OaiChatCompletionParams params = {                                */
/*       .model = "gpt-4o-mini",                                       */
/*       .messages = messages,                                         */
/*       .messages_len = 2,                                            */
/*   };                                                                */
/*                                                                     */
/*   OaiCompletionResult result;                                       */
/*   if (oai_chat_completions_create(&client, &params, &result) != 0)  */
/*       die("ungueltige argumente");                                  */
/*                                                                     */
/*   if (!result.ok) {                                                 */
/*       fprintf(stderr, "fehler (http %ld): %s\n",                    */
/*               result.http_status, result.error);                    */
/*   } else {                                                          */
/*       printf("%s\n",                                                */
/*           result.completion.choices[0].message.content);            */
/*   }                                                                 */
/*                                                                     */
/*   oai_completion_result_free(&result);                              */
/*   oai_client_free(&client);                                         */
/*                                                                     */
/* beispiel 2: mehrstufige konversation (verlauf weiterfuehren)        */
/*                                                                     */
/*   OaiMessage messages[] = {                                         */
/*       {.role = OAI_ROLE_USER, .content = "wie heisst die hauptstadt"}, */
/*   };                                                                */
/*   OaiChatCompletionParams params = {                                */
/*       .model = "gpt-4o-mini", .messages = messages,                 */
/*       .messages_len = 1,                                            */
/*   };                                                                */
/*                                                                     */
/*   OaiCompletionResult r1;                                           */
/*   oai_chat_completions_create(&client, &params, &r1);               */
/*                                                                     */
/*   // antwort an den verlauf haengen, dann weiterfragen:            */
/*   OaiMessage messages2[] = {                                        */
/*       messages[0],                                                  */
/*       {.role = OAI_ROLE_ASSISTANT,                                  */
/*        .content = r1.completion.choices[0].message.content},        */
/*       {.role = OAI_ROLE_USER, .content = "und einwohnerzahl?"},     */
/*   };                                                                */
/*   OaiChatCompletionParams params2 = {                               */
/*       .model = "gpt-4o-mini", .messages = messages2,                */
/*       .messages_len = 3,                                            */
/*   };                                                                */
/*   OaiCompletionResult r2;                                           */
/*   oai_chat_completions_create(&client, &params2, &r2);              */
/*                                                                     */
/*   oai_completion_result_free(&r2);                                  */
/*   oai_completion_result_free(&r1);                                  */
/*                                                                     */
/* beispiel 3: streaming (wie der async-iterator im npm-package)       */
/*                                                                     */
/*   // akkumulator fuer den kompletten text                           */
/*   // (buf_append: platzhalter fuer deinen eigenen akkumulator):     */
/*   static int on_chunk(const OaiChatCompletionChunk *chunk,          */
/*                       void *user_data)                              */
/*   {                                                                 */
/*       Buf *buf = user_data;                                         */
/*       for (size_t i = 0; i < chunk->choices_len; i++) {             */
/*           const OaiChunkChoice *c = &chunk->choices[i];             */
/*           if (c->content_delta != NULL)                             */
/*               buf_append(buf, c->content_delta);                    */
/*           if (c->finish_reason != NULL)                             */
/*               fprintf(stderr, "\n[finish: %s]\n",                   */
/*                              c->finish_reason);                     */
/*       }                                                             */
/*       return 0; // rueckgabe != 0 bricht den stream ab              */
/*   }                                                                 */
/*                                                                     */
/*   OaiStreamCallbacks cbs = {                                        */
/*       .on_chunk = on_chunk,                                         */
/*       .user_data = &buf,                                            */
/*   };                                                                */
/*   if (oai_chat_completions_create_stream(&client, &params, &cbs)    */
/*       != 0) die("ungueltige argumente");                            */
/*   // fehler kommen ueber cbs.on_error, nicht ueber den rueckgabe-   */
/*   // wert (der ist nur 0/-1 fuer benutzungsfehler wie NULL).        */
/*                                                                     */
/* beispiel 4: tool calls (function calling)                           */
/*                                                                     */
/*   OaiToolFunction weather_fn = {                                    */
/*       .name = "get_weather",                                        */
/*       .description = "aktuelles wetter fuer eine stadt",            */
/*       .parameters_json =                                            */
/*           "{\"type\":\"object\",\"properties\":{\"city\":"          */
/*           "{\"type\":\"string\"}},\"required\":[\"city\"]}",        */
/*   };                                                                */
/*   OaiTool tools[] = {{.function = weather_fn}};                     */
/*   OaiChatCompletionParams params = {                                */
/*       .model = "gpt-4o-mini", .messages = messages,                 */
/*       .messages_len = 1, .tools = tools, .tools_len = 1,            */
/*   };                                                                */
/*                                                                     */
/*   OaiCompletionResult result;                                       */
/*   oai_chat_completions_create(&client, &params, &result);           */
/*                                                                     */
/*   if (result.ok) {                                                  */
/*       OaiResponseMessage *m =                                       */
/*           &result.completion.choices[0].message;                    */
/*       for (size_t i = 0; i < m->tool_calls_len; i++)                */
/*           // m->tool_calls[i].name / .arguments (json-string) /     */
/*           // .id auswerten, ergebnis als tool-message zurueck:      */
/*           printf("tool %s(%s)\n", m->tool_calls[i].name,            */
/*                  m->tool_calls[i].arguments);                       */
/*   }                                                                 */
/*   oai_completion_result_free(&result);                              */
/*                                                                     */
/* beispiel 5: ollama statt openai (base_url umstellen)                */
/*                                                                     */
/*   OaiClientOptions opts = {                                         */
/*       .api_key = "ollama",       // wird meist ignoriert            */
/*       .base_url = "http://localhost:11434/v1",                      */
/*   };                                                                */
/*   // danach wie oben; model z.B. "llama3.1"                        */
/*                                                                     */
/* ------------------------------------------------------------------ */
#ifndef MAX_AGENT_OPENAI_COMPLETIONS
#define MAX_AGENT_OPENAI_COMPLETIONS

#include <stdbool.h>
#include <stddef.h>

/* ------------------------------------------------------------------ */
/* client: entspricht "new OpenAI({...})" im npm-package               */
/* ------------------------------------------------------------------ */

typedef struct {
    const char *api_key;  /* pflichtfeld (getenv("OPENAI_API_KEY"))     */
    const char *base_url; /* NULL => https://api.openai.com/v1          */
    long timeout_ms;      /* 0 => 600000 (npm-default); gilt nur fuer   */
                          /* nicht-streaming, streams haben nur einen   */
                          /* connect-timeout (10s)                      */
    int max_retries;      /* <0 => 2 (npm-default; nur bei verbindungs- */
                          /* fehlern, 408, 429 und 5xx)                 */
} OaiClientOptions;

typedef struct {
    char *api_key;
    char *base_url; /* ohne trailing slash */
    long timeout_ms;
    int max_retries;
    void *curl_handle; /* intern: wiederverwendeter easy-handle
                        * (keep-alive/tls-reuse). nicht thread-safe! */
} OaiClient;

/* rueckgabe 0 bei erfolg, -1 bei ungueligen argumenten/allocation    */
/* (nicht thread-safe: ein client gehoert in einen thread. mehrere    */
/* clients sind ok, auch gleichzeitig in verschiedenen threads.)      */
int oai_client_init(OaiClient *client, const OaiClientOptions *options);
void oai_client_free(OaiClient *client);

/* ------------------------------------------------------------------ */
/* anfrage-parameter: entspricht ChatCompletionCreateParams            */
/* ------------------------------------------------------------------ */

typedef enum {
    OAI_ROLE_DEVELOPER,
    OAI_ROLE_SYSTEM,
    OAI_ROLE_USER,
    OAI_ROLE_ASSISTANT,
    OAI_ROLE_TOOL,
} OaiRole;

/* tool-call: in antworten vom server geparst, in assistant-nachrichten*/
/* (tool-loop) als const-referenz zurueckgegeben                       */
typedef struct {
    char *id;        /* z.B. "call_abc123" */
    char *name;      /* funktionsname */
    char *arguments; /* json-argumente als string (vom modell erzeugt) */
} OaiToolCall;

typedef struct {
    OaiRole role;
    const char *content; /* NULL bei assistant-nachrichten mit tool_calls */
    const char *name;    /* optional */
    const char *tool_call_id; /* pflicht bei role == OAI_ROLE_TOOL */
    /* tool-loop: die tool_calls des assistant-antwort (aus            */
    /* OaiResponseMessage) beim naechsten request mit zurueckgeben.    */
    const OaiToolCall *tool_calls;
    size_t tool_calls_len;
} OaiMessage;

typedef struct {
    const char *name;            /* funktionsname */
    const char *description;     /* NULL erlaubt */
    const char *parameters_json; /* json-schema; NULL => leeres objekt */
} OaiToolFunction;

typedef struct {
    OaiToolFunction function; /* type ist immer "function" */
} OaiTool;

typedef struct {
    const char *model;
    const OaiMessage *messages;
    size_t messages_len;

    /* optionale felder, wie im npm-package (dort einfach weglassen): */
    bool has_temperature;
    double temperature;
    bool has_top_p;
    double top_p;
    bool has_n;
    int n;
    bool has_max_completion_tokens;
    int max_completion_tokens;
    bool has_max_tokens; /* legacy-feld */
    int max_tokens;
    const char *const *stop; /* array von stop-sequenzen */
    size_t stop_len;
    bool has_presence_penalty;
    double presence_penalty;
    bool has_frequency_penalty;
    double frequency_penalty;
    const OaiTool *tools;
    size_t tools_len;
} OaiChatCompletionParams;

/* ------------------------------------------------------------------ */
/* antworten: entspricht ChatCompletion (+ Stream-Chunks)              */
/* ------------------------------------------------------------------ */

typedef struct {
    char *role;    /* "assistant" */
    char *content; /* NULL wenn das modell nur tool-calls erzeugt hat */
    OaiToolCall *tool_calls;
    size_t tool_calls_len;
} OaiResponseMessage;

typedef struct {
    size_t index;
    OaiResponseMessage message;
    char *finish_reason; /* "stop" | "length" | "tool_calls" | ... */
} OaiChoice;

typedef struct {
    int prompt_tokens;
    int completion_tokens;
    int total_tokens;
} OaiUsage;

typedef struct {
    char *id;
    char *model;
    OaiChoice *choices;
    size_t choices_len;
    bool has_usage;
    OaiUsage usage;
} OaiChatCompletion;

/* ein stream-chunk entspricht einem ChatCompletionChunk aus dem       */
/* sse-strom: choices[i] enthaelt nur deltas, nicht den ganzen text.   */
typedef struct {
    size_t index;          /* position im ziel-tool_calls-array der nachricht */
    char *id;              /* nur im ersten delta des calls gesetzt           */
    char *name;            /* funktionsname, meist nur im ersten delta        */
    char *arguments_delta; /* argument-fragment (inkrementell)        */
} OaiChunkToolCall;

typedef struct {
    size_t index;
    char *role;          /* meist nur im ersten chunk gesetzt        */
    char *content_delta; /* inkrementeller text, NULL wenn keiner    */
    OaiChunkToolCall *tool_call_deltas;
    size_t tool_call_deltas_len;
    char *finish_reason; /* NULL ausser beim letzten chunk           */
} OaiChunkChoice;

typedef struct {
    char *id;
    char *model;
    OaiChunkChoice *choices;
    size_t choices_len;
} OaiChatCompletionChunk;

/* ------------------------------------------------------------------ */
/* api                                                                 */
/* ------------------------------------------------------------------ */

/* ergebnis einer nicht-streaming-anfrage. entspricht dem try/catch    */
/* um client.chat.completions.create() im npm-package: ok == false     */
/* ist APIError (http_status > 0) bzw. APIConnectionError              */
/* (http_status == 0), der fehlertext steht in error.                  */
typedef struct {
    bool ok;
    long http_status;             /* 0 bei verbindungsfehler */
    char *error;                  /* gesetzt wenn ok == false */
    OaiChatCompletion completion; /* gueltig wenn ok == true */
} OaiCompletionResult;

/* oai_chat_completions_create: entspricht                             */
/* client.chat.completions.create() ohne stream. fuellt result;        */
/* rueckgabe ist 0 (nutze result.ok), -1 nur bei NULL-argumenten oder  */
/* ungueltigem tool-parameters_json.                                   */
int oai_chat_completions_create(const OaiClient *client,
                                const OaiChatCompletionParams *params,
                                OaiCompletionResult *result);

typedef struct {
    void *user_data;
    /* wird pro sse-event aufgerufen; pointer sind nur waehrend des   */
    /* aufrufs gueltig. rueckgabe != 0 bricht den stream ab.          */
    int (*on_chunk)(const OaiChatCompletionChunk *chunk, void *user_data);
    /* wird bei fehlern aufgerufen (auch nach retries); http_status   */
    /* ist 0 bei verbindungsfehlern.                                  */
    void (*on_error)(long http_status, const char *message, void *user_data);
} OaiStreamCallbacks;

/* oai_chat_completions_create_stream: entspricht                     */
/* client.chat.completions.create({..., stream: true}). rueckgabe 0   */
/* auch bei http-fehlern (dann kam on_error), -1 bei NULL-argumenten  */
/* oder ungueltigem tool-parameters_json. endet der stream ohne       */
/* [DONE], ist das ein fehler (ueber on_error), keine halbe antwort.  */
int oai_chat_completions_create_stream(const OaiClient *client,
                                       const OaiChatCompletionParams *params,
                                       const OaiStreamCallbacks *callbacks);

/* ------------------------------------------------------------------ */
/* free-funktionen                                                     */
/* ------------------------------------------------------------------ */

/* ergebnis von oai_chat_completions_create (in-place, safe auf        */
/* zero-initialisierten structs)                                       */
void oai_completion_result_free(OaiCompletionResult *result);

/* ein einzelnes OaiChatCompletion (z.B. aus einem result kopiert)     */
void oai_chat_completion_free(OaiChatCompletion *completion);

/* einen einzelnen stream-chunk                                        */
void oai_chat_completion_chunk_free(OaiChatCompletionChunk *chunk);

#endif