#ifndef MAX_AGENT_EDITOR
#define MAX_AGENT_EDITOR

#include "config.h"

/* ------------------------------------------------------------------ */
/* /system-prompt: den system-prompt im $VISUAL/$EDITOR bearbeiten.    */
/*                                                                    */
/* die app zeichnet selbst in den normalen terminal-buffer (kein      */
/* alternate-screen) – derselbe buffer, in dem ein editor laeuft.     */
/* darum gilt: vorher saeubern, nachher neu aufsetzen. genau wie      */
/* restore()/screen_enter() das beim app-ende und app-start tun.      */
/* ------------------------------------------------------------------ */

/* effektiven system-prompt ermitteln: den aus der config, sonst
 * die eingebettete vorlage aus prompt.c. NULL/" = default- */
char *editor_effective_prompt(const Config *cfg);

/* externen editor mit <path> starten. erwartet: raw mode war schon
 * AUS und das terminal war schon sauber (screen_leave) – das erledigt
 * der aufrufer in cmd_system_prompt, damit die reihenfolge lesbar
 * bleibt. der editor erbt stdin/stdout/stderr vom terminal.
 *
 * rueckgabe: exit-code des editors (>=0), -1 wenn kein programm
 * gestartet werden konnte (fork/exec gescheitert). SIGTSTP im kind
 * (ctrl+z im editor) wird an uns selbst weitergereicht, damit das
 * job-control der shell sich kuemmert – seele von less/pine. */
int editor_run(const char *path);

#endif