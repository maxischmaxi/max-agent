#ifndef MAX_AGENT_CONTEXT
#define MAX_AGENT_CONTEXT

#include <stddef.h>
#include <stdint.h>

#include "chat.h"
#include "config.h"

/* ------------------------------------------------------------------ */
/* context: passt das transcript in das kontextfenster des modells.   */
/*                                                                    */
/* ohne tokenizer wird geschaetzt (~CTX_BYTES_PER_TOKEN bytes je      */
/* token). die schaetzung ist grob, aber sie muss nicht genau sein:   */
/* nach jeder antwort liefert die api mit OaiUsage.prompt_tokens die  */
/* WAHRE zahl fuer genau den request, den wir geschickt haben – aus   */
/* dem verhaeltnis beider zahlen entsteht ein korrekturfaktor, der    */
/* das budget der naechsten runde nachfuehrt (ctx_calibrate).         */
/* ------------------------------------------------------------------ */

/* grobe faustregel fuer englischen fliesstext; code und utf-8 mit
 * vielen multibyte-zeichen liegen darunter, die kalibrierung faengt
 * das ab */
#define CTX_BYTES_PER_TOKEN 4

/* struktur-aufschlag je nachricht (rolle, trennzeichen, json-
 * geruest) – die api zaehlt das mit, unser strlen nicht */
#define CTX_MSG_OVERHEAD 4

/* dito je tool-call: id, funktionsname-geruest, klammern */
#define CTX_CALL_OVERHEAD 8

/* "das modell gibt kein fenster an": dann wird nicht gekuerzt.
 * raten waere schlimmer als nichts tun – ein zu kleines budget
 * wuerde verlauf wegwerfen, den das modell haette sehen koennen. */
#define CTX_NO_LIMIT SIZE_MAX

/* korrekturfaktor in promille; 1000 = schaetzung unveraendert */
#define CTX_SCALE_ONE 1000

/* gemessene abweichung zwischen schaetzung und api-zaehlung. lebt
 * im AppState und ueberdauert damit die runden einer sitzung. */
typedef struct {
    size_t estimated;  /* unsere schaetzung des letzten requests */
    int prompt_tokens; /* was die api dafuer gezaehlt hat (0 = nie) */
    int scale;         /* promille: echt/geschaetzt, 0 = noch ungeeicht */
    size_t dropped;    /* nachrichten, die zuletzt weggelassen wurden */

    /* verbrauch der ganzen sitzung, fuer die statuszeile. gezaehlt
     * wird, was die api meldet – nicht unsere schaetzung. */
    size_t total_prompt;
    size_t total_completion;

    /* -------------------------------------------------------------- */
    /* compaction (send.c): statt verlauf still wegzukuerzen, wird    */
    /* alles bis zum watermark `covered` per llm zu einer summary    */
    /* verdichtet und ab dann mitgeschickt – der aufgabenkontext    */
    /* ueberlebt, wenn das fenster laeuft. summary gehoert dem struct */
    /* (ctx_reset), NULL = noch nichts komprimiert. das session-log  */
    /* legt beides als ereignis ab, ein resume stellt es wieder her. */
    /* compact_failed drosselt neuversuche: nach einem gescheiter-  */
    /* ten zusammenfassungs-call wird erst wieder versucht, wenn     */
    /* wieder nennenswert verlauf gefallen ist.                      */
    /* -------------------------------------------------------------- */
    char *summary;       /* llm-zusammenfassung, NULL = keine */
    size_t covered;      /* chat-index, bis zu dem die summary reicht */
    bool compact_failed; /* letzter versuch schlug fehl */
} CtxUsage;

/* compaction-zustand freigeben (summary, watermark, fehler-flag),
 * ohne die eichung (scale) und die gesamtzaehler anzutasten. */
void ctx_reset(CtxUsage *usage);

/* die summary in das format verpacken, in dem sie als USER-nachricht
 * mitgeschickt wird (pi-agenten-konvention: praefix + <summary>-tags).
 * heap-kopie des aufrufers, NULL bei OOM/NULL-eingang. */
char *ctx_summary_wrap(const char *summary);

/* geschaetzte tokens der verpackten summary (inkl. praefix und
 * nachricht-geruest). NULL/leer = 0. */
size_t ctx_summary_tokens(const char *summary);

/* tokens eines strings schaetzen (NULL = 0) */
size_t ctx_tokens_text(const char *text);

/* tokens einer nachricht schaetzen: text + tool-calls + aufschlag.
 * lokale rollen (ERROR, NOTICE) kosten 0 – sie werden nie gesendet. */
size_t ctx_tokens_message(const ChatMessage *msg);

/* tokens der tool-definitionen: sie haengen an JEDER anfrage und
 * gehen darum vom budget ab */
size_t ctx_tokens_tools(void);

/* schaetzung fuer genau den request, der ab index `from` gebaut
 * wird (system-prompt + summary + tool-definitionen + nachrichten).
 * das ist die vergleichsgroesse fuer ctx_calibrate. */
size_t ctx_tokens_request(const Chat *chat, size_t from,
                          const char *system_prompt, const char *summary);

/* wieviele tokens das transcript belegen darf: fenster des modells
 * minus antwort-reserve, tool-definitionen, system-prompt UND der
 * laufenden summary (sie ist fixer bestandteil jedes requests),
 * dann um den korrekturfaktor geschrumpft. CTX_NO_LIMIT, wenn das
 * modell kein contextWindow angibt (dann kuerzt ctx_trim_start nie).
 * usage darf NULL sein (= noch keine messung). */
size_t ctx_budget(const Model *model, const char *system_prompt,
                  const char *summary, const CtxUsage *usage);

/* index der ERSTEN nachricht, die noch gesendet wird. gezaehlt wird
 * von hinten, bis das budget voll ist.
 *
 * zwei regeln haelt die funktion dabei ein, sonst lehnt die api den
 * request ab:
 *  - die letzte nachricht geht immer mit, auch wenn sie allein
 *    schon zu gross ist (ein leerer request waere sinnlos)
 *  - eine angeschnittene tool-gruppe faellt ganz weg: ein
 *    tool-ergebnis ohne den assistant-call davor ist ungueltig
 *
 * budget == CTX_NO_LIMIT liefert immer 0. */
size_t ctx_trim_start(const Chat *chat, size_t budget);

/* compaction-cut (send.c): groesster gueltiger index, ab dem der
 * verlauf UNVERKUERZT mitgeschickt wird, wenn [cut, len) hoechstens
 * keep_tokens gross sein soll – die nachrichten davor wandern in
 * die summary. zwei garantien:
 *  - die letzte sendbare nachrichtengruppe bleibt IMMER draussen
 *    (der request braucht sie, egal wie klein keep_tokens ist)
 *  - alles vor `floor` (was ctx_trim_start sowieso fallen lassen
 *    wuerde) gehoert immer zur summary-seite
 * das ergebnis ist gruppen-ausgerichtet wie bei ctx_trim_start. */
size_t ctx_cut_point(const Chat *chat, size_t floor, size_t keep_tokens);

/* die echte token-zahl aus der antwort gegen unsere schaetzung
 * halten und den korrekturfaktor nachfuehren. prompt_tokens <= 0
 * oder estimated == 0 lassen den faktor unveraendert (die api hat
 * keine zahl geliefert). */
void ctx_calibrate(CtxUsage *usage, size_t estimated, int prompt_tokens);

/* den verbrauch der sitzung fortschreiben. bewusst getrennt von
 * ctx_calibrate: das eine eicht die schaetzung, das andere ist
 * buchhaltung fuer die anzeige. negative werte (api hat nichts
 * geliefert) werden ignoriert. */
void ctx_account(CtxUsage *usage, int prompt_tokens, int completion_tokens);

#endif
