#ifndef MAX_AGENT_SEND
#define MAX_AGENT_SEND

#include "config.h"
#include "openai_completions.h"
#include "state.h"

/* ChatRole -> OaiRole. CHAT_ROLE_ERROR ist keine api-rolle (nur
 * lokal gerendert): liefert -1 und wird beim bauen der nachrichten
 * uebersprungen. */
int send_role(ChatRole role);

/* das transcript als OaiMessage-array fuer params.messages bauen.
 * system_prompt (NULL = keiner) wird ALS ERSTE nachricht mit rolle
 * SYSTEM vorangestellt – der prompt ist request-kontext, kein
 * verlaufs-inhalt: er erscheint nicht im chat, und /clear rührt ihn
 * nicht an. ERROR-nachrichten fehlen, alle anderen rollen werden
 * gemappt. die content-strings werden NICHT kopiert, sondern als
 * const vom chat bzw. system_prompt geborgt: das array ist nur
 * gueltig, solange der chat nicht veraendert wird (anhaengen ist
 * ok, freigeben/klemmen nicht).
 * *out ist ein heap-array des aufrufers (free), rueckgabe ist die
 * anzahl; 0 bei leerem (oder nur-fehler-) chat, -1 bei OOM/NULL. */
int send_build_messages(const Chat *chat, const char *system_prompt,
                        OaiMessage **out);

/* wie send_build_messages, aber erst ab nachricht `from` – alles
 * davor hat context.c als nicht mehr ins fenster passend aussortiert
 * (ctx_trim_start liefert den index). der system-prompt bleibt
 * dabei immer erhalten: er steht nicht im transcript. */
int send_build_messages_from(const Chat *chat, size_t from,
                             const char *system_prompt, OaiMessage **out);

/* modell nach id (cfg->active_model) in der config suchen. liefert
 * das model und den zugehoerigen provider (api_key/base_url) oder
 * NULL, wenn die id in keinem provider existiert. */
const Model *send_find_model(const Config *cfg, const char *id,
                             const Provider **provider);

/* die unterhaltung abschicken (blocking, nicht-streaming; die UI
 * muss vorher einen busy-frame gezeichnet haben, denn waehrend des
 * requests wird nichts gerendert). die antwort – oder eine
 * fehlermeldung – wird an das transcript angehaengt, damit sie im
 * verlauf sichtbar bleibt.
 * rueckgabe: 0 bei antwort/fehler aus der api, -1 bei
 * benutzungsfehlern (kein modell/provider/api-key gewaehlt, OOM).
 * fehler landen IMMER als CHAT_ROLE_ERROR im verlauf. */
int send_message(AppState *state, const Config *cfg);

/* ------------------------------------------------------------------ */
/* haken, die send_stream in die UI zurueckruft. beide bekommen den  */
/* gemeinsamen ctx – die streaming-schleife selbst weiss nichts von  */
/* terminal, tasten oder layout.                                      */
/* ------------------------------------------------------------------ */
typedef struct {
    void *ctx;
    /* nach chunks aufrufen, damit der aufrufer neu zeichnet.
     * pflicht: ohne redraw sieht man vom stream nichts. */
    void (*redraw)(void *ctx);
    /* rueckfrage vor einem tool, das etwas veraendert (siehe
     * tool_needs_confirm). true = ausfuehren, false = ablehnen.
     * NULL heisst: alles laeuft ungefragt durch – das ist der
     * modus fuer tests und nicht-interaktive aufrufer. */
    bool (*confirm_tool)(const char *name, const char *arguments, void *ctx);
} SendHooks;

/* wie send_message, aber als stream: vor dem request entsteht eine
 * leere ASSISTANT-nachricht als platzhalter, jeder chunk haengt an
 * ihren text – die antwort waechst also live im verlauf.
 * hooks->redraw wird nach chunks aufgerufen, damit der rufende das
 * UI aktualisieren kann (send selbst drosselt auf SEND_REDRAW_MS;
 * das erste fragment zeichnet sofort). ein fehler wirft einen
 * leeren platzhalter weg, teil-antworten bleiben im verlauf und
 * bekommen die fehlermeldung hinterher.
 * rueckgabe: 0 wenn der stream durchlief, -1 sonst (fehlermeldung
 * steht als ERROR-nachricht im verlauf). */
int send_stream(AppState *state, const Config *cfg, const SendHooks *hooks);

#endif