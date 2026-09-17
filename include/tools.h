#ifndef MAX_AGENT_TOOLS
#define MAX_AGENT_TOOLS

#include <stdbool.h>
#include <stddef.h>

#include "openai_completions.h"

/* ------------------------------------------------------------------ */
/* tool-registry: das, was das modell im agent-loop aufrufen darf.   */
/* die OaiTool-definitionen (name, beschreibung, json-schema) gehen   */
/* direkt in die anfrage; die ausfuehrung passiert lokal und         */
/* ungefragt – der benutzer hat der ki den rechner ueberlassen,       */
/* abbruch gibt es nur per esc/ctrl+c beim request.                   */
/* ------------------------------------------------------------------ */

/* alle registrierten tools (statisch, lebt solange die app) */
const OaiTool *tool_registry(size_t *len);

/* tool nach name suchen; NULL wenn unbekannt */
const OaiTool *tool_find(const char *name);

/* gehoert dieses tool zu den datei-mutierenden, die NIE parallel
 * laufen duerfen (write_file, edit_file)? der agent-loop (send.c)
 * fragt das fuer jede runde ab: ist EIN call dabei, laeuft der
 * ganze stapel sequenziell – wie der pi-agent seine batches mit
 * sequential-tools behandelt. */
bool tool_is_sequential(const char *name);

/* den aktuell laufenden tool-call abbrechen (kill an die prozess-
 * gruppe des bash-kinds). aufrufer: der agent-loop beim abbruch;
 * ohne laufendes tool ein no-op. der worker-thread kehrt nach dem
 * kill umgehend zurueck. */
void tool_kill_current(void);

/* ein tool ausfuehren. arguments_json ist der json-string, den das
 * modell erzeugt hat. rueckgabe: ergebnis als heap-string des
 * aufrufers – auch fehlerfaelle (unbekanntes tool, kaputtes json,
 * fehlende argumente, gescheitertes kommando) kommen als text
 * zurueck, denn die api erwartet das tool-ergebnis als string.
 * NULL nur bei OOM. */
char *tool_execute(const char *name, const char *arguments_json);

/* tool-output-grenzen (wie der pi-agent): hoechstens 2000 zeilen
 * oder 50 KB, je nachdem, was zuerst kommt. read_file behaelt den
 * ANFANG und zeigt mit offset weiter; bash behaelt das ENDE –
 * fehler und ergebnisse stehen am output-ende – und legt den
 * vollstaendigen output in eine temp-datei, deren pfad im ergebnis
 * steht. */
#define TOOL_MAX_LINES 2000
#define TOOL_MAX_BYTES ((size_t)50 * 1024)

/* bash: soviel output wird maximal eingesammelt. was darueber
 * hinausgeht, wird verworfen (das kind darf nie auf einem vollen
 * pipe-buffer haengen bleiben); der hinweis meldet das. die
 * temp-datei enthaelt dann eben genau diesen ausschnitt. */
#define TOOL_BASH_COLLECT_MAX ((size_t)16 * 1024 * 1024)

#endif
