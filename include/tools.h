#ifndef MAX_AGENT_TOOLS
#define MAX_AGENT_TOOLS

#include <stdbool.h>
#include <stddef.h>

#include "openai_completions.h"

/* ------------------------------------------------------------------ */
/* tool-registry: das, was das modell im agent-loop aufrufen darf.   */
/* die OaiTool-definitionen (name, beschreibung, json-schema) gehen   */
/* direkt in die anfrage; die ausfuehrung passiert lokal.            */
/*                                                                    */
/* lesende tools laufen ungefragt; alles, was schreibt oder eine    */
/* shell startet, fragt vorher nach (tool_needs_confirm + die        */
/* confirm-hook in send.h).                                          */
/* ------------------------------------------------------------------ */

/* alle registrierten tools (statisch, lebt solange die app) */
const OaiTool *tool_registry(size_t *len);

/* tool nach name suchen; NULL wenn unbekannt */
const OaiTool *tool_find(const char *name);

/* muss der benutzer zustimmen, bevor dieses tool laeuft?
 * lesen ist harmlos, schreiben und shell-kommandos nicht – die
 * kann ein modell nicht zurueckholen. unbekannte namen gelten als
 * bestaetigungspflichtig (im zweifel fragen). */
bool tool_needs_confirm(const char *name);

/* ein tool ausfuehren. arguments_json ist der json-string, den das
 * modell erzeugt hat. rueckgabe: ergebnis als heap-string des
 * aufrufers – auch fehlerfaelle (unbekanntes tool, kaputtes json,
 * fehlende argumente, gescheitertes kommando) kommen als text
 * zurueck, denn die api erwartet das tool-ergebnis als string.
 * output wird auf TOOL_MAX_OUT bytes gekappt. NULL nur bei OOM. */
char *tool_execute(const char *name, const char *arguments_json);

/* wie gross ein tool-ergebnis hoechstens wird (rest abgeschnitten,
 * mit hinweis) – schuetzt den model-kontext vor riesigen outputs */
#define TOOL_MAX_OUT ((size_t)64 * 1024)

#endif