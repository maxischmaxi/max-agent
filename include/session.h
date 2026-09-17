#ifndef MAX_AGENT_SESSION
#define MAX_AGENT_SESSION

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

#include "chat.h"
#include "config.h"
#include "context.h"

/* ------------------------------------------------------------------ */
/* session: eine unterhaltung als JSONL-transcript auf der platte.    */
/*                                                                    */
/* format: pro session zwei dateien in                                */
/*   ~/.config/.maxagent/sessions/                                     */
/*     <id>.json  – meta (id, name, zeiten, snapshots, cwd)           */
/*     <id>.jsonl – transcript, EINE zeile pro ereignis (append-only) */
/*                                                                    */
/* jede session gehoert zu dem verzeichnis, in dem sie gestartet      */
/* wurde (meta-feld "cwd"): der resume-dialog zeigt nur die           */
/* sessionen des ordners, in dem die app laeuft – wer in einem        */
/* projekt arbeitet, sieht nur dessen unterhaltungen. metas ohne      */
/* "cwd" (format version 1) bekommen beim app-start das aktuelle      */
/* arbeitsverzeichnis zugewiesen (sessions_migrate_legacy).           */
/*                                                                    */
/* JSONL statt einer grossen datei oder sqlite: append ist ein   */
/* einziger fwrite (O(1), nicht O(dateigroesse)), keine externe    */
/* abhaengigkeit – cJSON ist eh da –, resume liest genau eine     */
/* datei, und eine abgerissene letzte zeile (kein '\n' am ende)  */
/* wird beim laden erkannt und uebersprungen. die session-liste    */
/* scannt nur die kleinen meta-dateien.                            */
/* ------------------------------------------------------------------ */

/* versions-stempel im meta-file: version 1 kannte kein "cwd",
 * version 2 schreibt es und migratiert alte bestände nach
 * (sessions_migrate_legacy). */
#define SESSION_VERSION "2"

/* "s-" + hex(unix-ms) + "-" + 6 hex-zufall + '\0' */
#define SESSION_ID_MAX 24

typedef struct {
    bool active; /* zeichnet die app gerade in diese session? */
                 /* false = keine session offen; die dateien    */
                 /* einer beendeten session bleiben liegen.     */
    char id[SESSION_ID_MAX];
    char *name;           /* NULL = unbenannt (via /rename setzbar) */
    long long created_at; /* unix-millisekunden */
    long long updated_at; /* dito, bei jedem ereignis nachgefuehrt */
    long long worked_ms;  /* kumulierte zeit, die die ki in dieser */
                          /* session gearbeitet hat (alle turns)  */
    size_t messages;      /* ereignisse im transcript (list-anzeige) */
    char *model;          /* snapshot des modells bei session_start */
    char *base_url;       /* dito provider-url */
    char *system_prompt;  /* NULL = default, "" = aus, text = eigener */
    char *cwd;            /* verzeichnis, in dem die session gestartet */
                          /* wurde. bestimmt, wo der resume-dialog    */
                          /* sie anzeigt. NULL = legacy ohne cwd.     */
    FILE *log;            /* <id>.jsonl, offen solange active */
} Session;

/* eintrag der session-liste (resume-dialog). nur die felder, die
 * die liste auch anzeigt; das transcript selbst wird erst beim
 * auswaehlen gelesen. */
typedef struct {
    char id[SESSION_ID_MAX];
    char *name;           /* NULL = unbenannt */
    char *preview;        /* gekuerzte erste user-nachricht, NULL = keine */
    long long updated_at; /* unix-ms, sortierung der liste */
    size_t messages;
} SessionInfo;

typedef struct {
    SessionInfo *items;
    size_t len;
} SessionList;

/* sessions-verzeichnis anlegen (idempotent). main ruft das beim
 * start; session_start/session_list_load rufen es zur sicherheit
 * nochmal. */
int session_dir_ensure(void);

/* migration alter bestände: metas im sessions-verzeichnis ohne
 * "cwd"-feld bekommen das aktuelle arbeitsverzeichnis als ordner
 * und version "2". idempotent (dateien mit cwd bleiben unberührt)
 * und best effort – ein kaputtes meta bricht die migration der
 * anderen nicht ab. rueckgabe 0 = alle gelesen und ggf. migrant. */
int sessions_migrate_legacy(void);

/* neue session: id generieren, meta schreiben, log oeffnen. die
 * snapshots (model, base_url, system_prompt) stammen aus cfg, das
 * cwd aus dem aktuellen arbeitsverzeichnis. */
int session_start(Session *s, const Config *cfg);

/* bestehende session anhaengend oeffnen (resume). meta wird gelesen,
 * das transcript NICHT – dafuer gibt es session_read_transcript. */
int session_open(Session *s, const char *id);

/* session beenden: log schliessen, meta finalisieren, felder
 * freigeben. die dateien bleiben genau so liegen, wie sie sind –
 * "neue session starten" veraendert die alte nicht. */
void session_end(Session *s);

/* wie session_end; name nur der deutlichkeit halber (app-exit). */
void session_free(Session *s);

/* name setzen (leerer name = fehler). schreibt das meta sofort. */
int session_rename(Session *s, const char *name);

/* prompt-snapshot der offenen session ersetzen: der benutzer kann
 * den system-prompt mitten in einer session aendern, und ein resume
 * soll den prompt wiederherstellen, den die session ZULETZT hatte –
 * nicht den von ihrem anfang. NULL = default, "" = aus, text. */
int session_prompt_changed(Session *s, const char *prompt);

/* ------------------------------------------------------------------ */
/* ereignis-log. alle funktionen sind no-ops ohne aktive session und  */
/* schlagen still fehl – das aufzeichnen darf den chat niemals       */
/* blockieren. fehler bei einer einzelnen zeile verlieren hoechstens  */
/* diese eine, nicht die session.                                     */
/* ------------------------------------------------------------------ */
int session_log_user(Session *s, const char *text);

/* die kumulierte arbeitszeit der session setzen (turn-ende, wenn
 * die ki fertig ist) und ins meta schreiben */
void session_worked_set(Session *s, long long worked_ms);

/* eine (ggf. noch teil-) antwort des modells. calls = die tool-
 * calls, die daran haengen (NULL/0 = keine). ttft/total in ms,
 * <0 = nicht gemessen. work_ms = die zeit des GESAMTEN turns seit
 * der user-nachricht (thinking + alle runden + tools), <0 = un-
 * bekannt. round = agent-loop-runde, <0 = unbekannt.
 * token-zaehlung <0 = die api hat nichts geliefert. */
int session_log_assistant(Session *s, const char *text,
                          const ChatToolCall *calls, size_t calls_len,
                          long long ttft_ms, long long total_ms,
                          long long work_ms, int round, const char *model,
                          int prompt_tokens, int completion_tokens,
                          bool aborted);

/* ergebnis eines tool-aufrufs. dur_ms = ausfuehrungsdauer. */
int session_log_tool(Session *s, const char *call_id, const char *name,
                     const char *result, long long dur_ms);

/* lokale fehlermeldung (http_status 0 = verbindungs-/setup-fehler) */
int session_log_error(Session *s, const char *text, long http_status);

/* hinweis der app (verlauf gekuerzt, abgebrochen, ...) */
int session_log_notice(Session *s, const char *text);

/* compaction-ereignis: die llm hat den verlauf bis chat-index
 * `covered` zur text-zusammenfassung `summary` verdichtet. beim
 * replay (replay_line) landet beides im CtxUsage und ersetzt dort
 * eine evtl. aeltere summary – es gibt immer nur die letzte. */
int session_log_compaction(Session *s, const char *summary, size_t covered);

/* ------------------------------------------------------------------ */
/* session-liste fuer den resume-dialog                               */
/* ------------------------------------------------------------------ */

/* die sessionen EINES verzeichnisses einlesen (nach updated_at
 * absteigend): nur metas, deren "cwd" mit dir uebereinstimmt.
 * dir == NULL = aktuelles arbeitsverzeichnis. 0 auch bei leerer
 * liste; -1 nur bei harten fehlern (dann ist out leer). */
int session_list_load(SessionList *out, const char *dir);
void session_list_free(SessionList *l);

/* prefix-match auf name, id und preview – dieselbe semantik wie
 * models_match: leerer suchtext trifft alles. */
int sessions_match(const SessionList *l, const char *search, int *out,
                   int out_max);

/* transcript einer geoeffneten session in einen (leeren) chat
 * zurueckspielen: nachrichten, tool-calls, tool-ergebnisse, fehler
 * und hinweise. die ctx-gesamtzaehler werden aus den gespeicherten
 * token-zahlen rekonstruiert (ctx == NULL = nicht interessiert). */
int session_read_transcript(const Session *s, Chat *chat, CtxUsage *ctx);

#endif