#ifndef MAX_AGENT_DEBUG
#define MAX_AGENT_DEBUG

#include <stdbool.h>

/* ------------------------------------------------------------------ */
/* debug-log: mit --debug startet die app eine trace-datei unter     */
/* /tmp und schreibt ALLES relevante hinein – als mensch oder agent   */
/* kann man danach exakt nachvollziehen, was passiert ist, und bei    */
/* einem haenger zeigt die letzte zeile, wo es klemmte.               */
/*                                                                    */
/*  - die datei heisst zuerst /tmp/max-agent-debug-<pid>.log und wird */
/*    sobald die session entsteht auf /tmp/max-agent-<session-id>.log */
/*    umbenannt (der inhalt bleibt)                                   */
/*  - jede zeile: wand-uhr, monotone uhr, thread-id, ereignis          */
/*  - thread-sicher (flockfile) – worker-threads loggen mit           */
/*  - in einem asan-build (make, default) werden asan-berichte       */
/*    (use-after-free, leaks, ...) zusaetzlich in die datei          */
/*    umgeleitet                                                       */
/* ------------------------------------------------------------------ */

/* debug einschalten (main: --debug). path = ziel-datei. */
void dbg_init(const char *path);

/* umbenennen, sobald die session-id feststeht (session.c). */
void dbg_rename(const char *session_id);

/* ereignis ins log. ohne dbg_init ein no-op (fast-path). */
void dbg(const char *fmt, ...);

/* --debug gesetzt? (fuer den hinweis in der statuszeile) */
bool dbg_active(void);

/* aktueller pfad der log-datei ("" ohne --debug). aktualisiert
 * sich mit dbg_rename – draw schreibt ihn in die statuszeile */
const char *dbg_path(void);

#endif