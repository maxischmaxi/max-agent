#ifndef MAX_AGENT_PROMPT
#define MAX_AGENT_PROMPT

#include "config.h"

/* system-prompt fuer die session bauen. heap-string des aufrufers,
 * NULL bei allocation-fehler.

 * cfg->system_prompt entscheidet:
 *   NULL  -> eingebaute default-vorlage (identitaet, heutiges
 *            datum, arbeitsverzeichnis, arbeitsregeln; aufgebaut
 *            wie der system-prompt des pi-agenten)
 *   ""    -> explizit KEIN system-prompt
 *   sonst -> genau dieser text, unveraendert */
char *prompt_build(const Config *cfg);

#endif