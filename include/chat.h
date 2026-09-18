#ifndef MAX_AGENT_CHAT
#define MAX_AGENT_CHAT

#include <stdbool.h>
#include <stddef.h>

#include "input.h"

/* ------------------------------------------------------------------ */
/* chat: das transcript der unterhaltung. das ist DIE datenstruktur  */
/* hinter AppState, an der senden, streaming und spaeter der agent- */
/* loop mit tool-calls haengen: eine append-only liste von nach-    */
/* richten. alle strings sind heap-kopien und gehoeren dem chat.    */
/* ------------------------------------------------------------------ */

typedef enum {
    CHAT_ROLE_SYSTEM,    /* system-prompt (spaeter aus der config)   */
    CHAT_ROLE_USER,      /* was der benutzer getippt hat             */
    CHAT_ROLE_ASSISTANT, /* antwort des modells                     */
    /* keine echte chat-rolle: lokal erzeugte meldung (z.B. HTTP-
     * fehler beim senden). wird nur gerendert, nie an die api
     * geschickt; daher bei der umwandlung in OaiMessage[] ueber-
     * springen. */
    CHAT_ROLE_ERROR,
    /* ergebnis eines tool-aufrufs: gehoert als antwort auf einen
     * tool_call zur unterhaltung und MUSS mit zurueckgeschickt
     * werden (tool_call_id verweist auf den call). */
    CHAT_ROLE_TOOL,
    /* wie ERROR keine api-rolle: hinweis der app an den benutzer
     * (z.B. "verlauf gekuerzt"). wird gerendert, aber nie gesendet
     * und zaehlt nicht ins token-budget. */
    CHAT_ROLE_NOTICE,
} ChatRole;

/* geht diese rolle an die api? ERROR und NOTICE sind lokale
 * meldungen: sie werden nur gezeichnet, nie gesendet – und sie
 * kosten deshalb auch keine tokens (context.c). */
bool chat_role_sent(ChatRole role);

/* ein tool-call des modells: name + argumente als json-string.
 * WICHTIG: identisch aufgebaut wie OaiToolCall aus
 * openai_completions.h (gleiche feldreihenfolge/-typen) – send.c
 * castet das array direkt (siehe _Static_assert dort). bei einer
 * aenderung hier MUSS OaiToolCall mitgeaendert werden. */
typedef struct {
    char *id;        /* z.B. "call_abc123" */
    char *name;      /* funktionsname, z.B. "bash" */
    char *arguments; /* json-argumente als string (vom modell) */
} ChatToolCall;

/* einrueckung der fortsetzungs-zeilen eines umgebrochenen tool-
 * calls (z.B. bash mit sehr langen parametern). chat_wrap plant
 * die umbrueche mit dieser breite, row_tool_call rueckt genau so
 * weit ein – beide muessen dieselbe zahl sehen. */
#define TOOL_INDENT_W 2

/* darstellungs-string eines tool-calls: pfeil + name + argumente,
 * genau so, wie row_tool_call ihn zeichnet. json-unicode-escapes
 * in den argumenten (\u0026 -> &, \u00e4 -> ae-umlaut) werden fuer
 * DIE ANZEIGE dekodiert – die rohen argumente bleiben immer
 * unangetastet: sie gehen 1:1 an die api zurueck (round-trip im
 * agent-loop) und ins session-log. die ChatLine.off/len der
 * tool-zeilen aus chat_wrap verweisen auf genau diesen string;
 * der renderer baut ihn sich je zeile wieder auf. NULL nur bei
 * OOM (dann stirbt die app eh). */
char *chat_tool_display(const ChatToolCall *call);

/* wie weit die escape-dekodierung der ANZEIGE geht (roh bleibt immer):
 * TOOLS = nur druckbare \uXXXX-escapes – die argument-zeile eines
 * tool-calls ist EINE zeile, steuerzeichen wuerden das layout
 * zerstoeren; TEXT = vollstaendig (\n -> newline, \t -> 4 spacen,
 * \" \' \\ \/ -> zeichen, \uXXXX auch fuer steuerzeichen) –
 * nachrichtentext wird erst dekodiert und dann von chat_wrap in
 * zeilen zerlegt. */
typedef enum {
    ESC_DECODE_TOOLS = 0,
    ESC_DECODE_TEXT,
} EscDecodePolicy;

/* json-artige escapes (\n, \t, \u00e4, \\, ...) im string NUR fuer
 * die darstellung dekodieren: liefert eine frische heap-kopie (NULL
 * bei NULL-eingang oder allocation-fehler), der eingang bleibt
 * unangetastet. unbekannte escapes, kaputte \uXXXX und lone
 * backslashes bleiben literal stehen. */
char *chat_decode_escapes(const char *s, EscDecodePolicy policy);

typedef struct {
    ChatRole role;
    char *text; /* heap-kopie, utf-8, mehrzeilig ('\n'-getrennt) */

    /* ASSISTANT: thinking des modells ("reasoning_content"), das
     * vor dem eigentlichen text gestreamt wurde. NULL = keins.
     * wird mit zurueckgeschickt (agent-loop), im session-log
     * aufgezeichnet und im chat dim gerendert – aber NIEMALS in
     * m->text eingemischt: api-round-trip und log sehen text und
     * reasoning getrennt. */
    char *reasoning;

    /* ASSISTANT: tool-calls, die das modell ausfuehren will
     * (agent-loop). text kann dabei leer sein. */
    ChatToolCall *tool_calls;
    size_t tool_calls_len;

    /* TOOL: id des calls, auf den dieses ergebnis antwortet */
    char *tool_call_id;
} ChatMessage;

typedef struct {
    ChatMessage *msgs; /* NULL solange leer */
    size_t len;
    size_t cap;
} Chat;

/* nachricht hinten anhaengen; text wird kopiert ("" ist erlaubt,
 * z.B. als platzhalter-nachricht, die das streaming fuellt).
 * '\r'-bytes werden gefiltert – sie wuerden beim zeichnen den
 * cursor an den zeilenanfang zurueckwerfen. rueckgabe 0 bei
 * erfolg, -1 bei NULL-text oder allocation. */
int chat_append(Chat *chat, ChatRole role, const char *text);

/* tool-ergebnis anhaengen: rolle TOOL, text = ergebnis,
 * tool_call_id verweist auf den beantworteten call. */
int chat_append_tool(Chat *chat, const char *tool_call_id, const char *result);

/* tool-calls an die LETZTE nachricht uebergeben (stream-ende).
 * das array samt seiner strings wechselt in den besitz des
 * chats; aufrufer gibt danach nichts mehr frei. NULL/0 entfernt
 * vorhandene tool-calls der letzten nachricht. */
int chat_set_tool_calls(Chat *chat, ChatToolCall *calls, size_t len);

/* letzte nachricht entfernen (text wird freigegeben). false, wenn
 * das transcript leer ist. */
bool chat_pop(Chat *chat);

/* text an die LETZTE nachricht anhaengen (streaming: die antwort
 * waechst in den platzhalter hinein). leerer text ist ein no-op
 * mit rueckgabe true; false bei leerem chat, NULL-text oder
 * allocation-fehler. */
bool chat_append_text(Chat *chat, const char *text);

/* thinking-fragment an die LETZTE nachricht anhaengen (streaming:
 * "reasoning_content"-deltas). die nachricht muss ein assistant-
 * platzhalter sein, sonst false. '\r' wird wie ueberall gefiltert.
 * leerer text ist ein no-op mit rueckgabe true. */
bool chat_append_reasoning(Chat *chat, const char *text);

/* reasoning der LETZTEN nachricht ERSETZEN, ownership wechselt in
 * den chat (nicht-streaming-pfad und transcript-replay: dort ist
 * das thinking in einem stueck da, statt als deltas). NULL/leer
 * entfernt vorhandenes reasoning. -1 bei leerem chat/OOM. */
int chat_set_reasoning(Chat *chat, char *reasoning);

/* transcript leeren: alle texte freigeben, kapazitaet behalten.
 * das ist das, was /clear aufrufen wird. */
void chat_clear(Chat *chat);

/* alles freigeben, inkl. des arrays selbst. danach ist der struct
 * wieder zero-initialisierbar (bzw. sofort wiederverwendbar). */
void chat_free(Chat *chat);

/* den mehrzeiligen chat-input zu einem flachen string joinen
 * (zeilen durch '\n' getrennt). NULL bei leerer eingabe oder
 * allocation-fehler; sonst heap-string des aufrufers. */
char *chat_flatten_input(const Input *in);

/* ------------------------------------------------------------------ */
/* render-zeilen: chat_wrap zerlegt das transcript in sichtbare     */
/* zeilen (word-wrap am leerzeichen, nur ueberlange woerter werden  */
/* hart an der breite gebrochen). jede zeile verweist per byte-     */
/* offset/laenge auf den original-text – nichts wird kopiert.      */
/* die tabelle wird pro frame neu berechnet und nur zum zeichnen    */
/* benutzt; sie wird nirgendwo gecacht.                            */
/* ------------------------------------------------------------------ */

typedef struct {
    size_t msg;       /* index der nachricht in msgs      */
    size_t off;       /* byte-offset des zeilenanfangs im text */
    size_t len;       /* byte-laenge dieser render-zeile  */
    bool first;       /* erste zeile der nachricht         */
    bool lstart;      /* beginnt am originalen zeilenan-  */
                      /* fang (nach '\n'): nur dann darf  */
                      /* der markdown-scanner ansetzen    */
                      /* (siehe markdown.h). bei 'first'  */
                      /* implizit true                    */
    ChatRole role;    /* kopie der nachrichten-rolle: die   */
                      /* slot-mapping braucht sie ohne state-pointer */
    int tool;         /* -1 = textzeile; -2 = tabellen-zeile */
                      /* (off/len im tabellen-display-string, */
                      /* blk_start/blk_end = block-grenzen   */
                      /* im originaltext); -3 = thinking-    */
                      /* zeile (dim gerendert, off/len wie   */
                      /* textzeilen); sonst index in         */
                      /* msgs[msg].tool_calls: render-zeile  */
                      /* des calls (off/len unbenutzt)       */
    size_t blk_start; /* tool==-2: tabelle im originaltext  */
    size_t blk_end;   /* (beide byte-offsets, end exklusiv) */
} ChatLine;

/* alle render-zeilen des transcripts fuer eine text-breite (in
 * sichtbaren zellen; label/einrueckung zaehlen nicht mit) berechnen.
 * gefuellt werden hoechstens out_max eintraege in out (out darf NULL
 * sein), die rueckgabe ist die GESAMTZEilenzahl – bei > out_max muss
 * der aufrufer mit groesserer arena erneut aufrufen. breite je
 * codepoint: 1 zelle (keine wcwidth-tabelle). */
size_t chat_wrap(const Chat *chat, int width, ChatLine *out, size_t out_max);

/* die anzeige-kopie des LETZTEN chat_wrap-aufrufs: alle texte des
 * gewrappten bereichs dekodiert hintereinander. die ChatLine.off
 * (tool == -1) zeigen hinein – der renderer liest texte NIE mehr
 * direkt aus ChatMessage. chat_disp_off(mi) = start der nachricht
 * mi in der kopie (fuer block-scan, tabellen, fence-highlight).
 * gueltig bis zum naechsten chat_wrap (je frame neu). */
const char *chat_disp_text(void);
size_t chat_disp_off(size_t mi);
void chat_free_disp(void);

#endif