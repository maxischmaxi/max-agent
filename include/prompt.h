#ifndef MAX_AGENT_PROMPT
#define MAX_AGENT_PROMPT

/* den (fest eingebauten) system-prompt fuer die session bauen.
 * heap-string des aufrufers (free!), NULL bei allocation-fehler.
 * datum und arbeitsverzeichnis werden pro request eingesetzt –
 * laeuft eine session ueber mitternacht, loggt sie automatisch
 * das neue datum. der prompt ist absichtlich NICHT konfigurierbar:
 * er ist teil der codebase. */
char *prompt_build(void);

#endif