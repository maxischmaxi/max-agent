#ifndef MAX_AGENT_PROMPT
#define MAX_AGENT_PROMPT

/* den system-prompt fuer die session bauen. heap-string des
 * aufrufers (free!), NULL bei allocation-fehler. datum und
 * arbeitsverzeichnis werden pro request neu eingesetzt – laeuft
 * eine session ueber mitternacht, loggt sie automatisch das neue
 * datum. die vorlage selbst ist fest in der codebase (src/prompt.c)
 * und nicht konfigurierbar.
 *
 * ZUSAETZLICH werden projektkontext-dateien (AGENTS.md) angehaengt,
 * exakt wie der pi-agent: global aus dem config-verzeichnis
 * (~/.config/.maxagent) und aus dem projekt-verzeichnis (cwd beim
 * start). pro verzeichnis gewinnt die erste dieser dateien:
 * AGENTS.override.md, AGENTS.md, AGENTS.MD, CLAUDE.md, CLAUDE.MD.
 * der inhalt landet byte-identisch zum pi-format in einem
 * <project_context>-abschnitt am ende des prompts. fehlen beide,
 * gibt es den abschnitt nicht. */
char *prompt_build(void);

#endif