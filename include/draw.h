#ifndef MAX_AGENT_DRAW
#define MAX_AGENT_DRAW

#include <stdio.h>

#include "chat.h"
#include "config.h"
#include "state.h"
#include "utils.h"

/* ------------------------------------------------------------------ */
/* inkrementeller scrollback-renderer (so arbeitet auch pi): es gibt    */
/* KEINEN alternativ-bildschirm und kein vollbild-neuzeichnen.        */
/*                                                                    */
/*  - fertiger chat-inhalt wird genau EINMAL gedruckt und scrollt     */
/*    dann natuerlich ins terminal-scrollback hoch – tmux-history,    */
/*    mausrad und kopieren funktionieren, weil alles im normalen      */
/*    puffer landet                                                   */
/*  - die neueste nachricht steht dadurch immer direkt ueber dem     */
/*    unten angedockten bereich, nie am oberen rand                  */
/*  - "live" neu gezeichnet werden nur zwei dinge: die aktuelle       */
/*    streaming-zeile (die letzte, noch waechsende zeile der ant-    */
/*    wort bzw. der thinking-hinweis) und der dock: eingabefeld,      */
/*    befehlsliste, dialoge und die zwei statuszeilen                */
/*  - der renderer ist rein relativ: der cursor parkt nach jedem     */
/*    frame auf der letzten dock-zeile, im naechsten frame werden     */
/*    genau die vorherigen live+dock-zeilen geraeumt, der neue block  */
/*    gedruckt und die zwischenzeilen-\n scrollen am bildschirm-     */
/*    rand ganz von selbst – eine absolute cursor-position wird       */
/*    nie gebraucht                                                  */
/* ------------------------------------------------------------------ */

/* einen frame rendern: neuen chat-inhalt drucken, live-zeile und
 * dock neu zeichnen. rows/cols ist die terminalgroesse (der dock
 * klemmt seine groesse daran). */
void draw(int rows, int cols, AppState *state, const Config *cfg);

/* nach SIGWINCH: das terminal hat umgebrochen, der relative
 * cursor-zustand ist unbrauchbar – bis zum boden scrollen und den
 * dock frisch aufsetzen. */
void draw_reset(int rows);

/* leichter frame waehrend die ki arbeitet: nur die spinner-zeile(n)
 * werden in ort und stelle ueberschrieben, der rest des docks
 * bleibt unberuehrt. der watchdog feuert ~10 ticks/s – ein voller
 * frame je tick liesse die input-leiste flackern. ohne gueltiges
 * layout (resize) oder ohne laufenden turn: voller draw(). */
void draw_busy_tick(int rows, int cols, AppState *state, const Config *cfg);

/* der chat-inhalt wurde komplett ersetzt (/new, resume): alles
 * bisher gedruckte bleibt im scrollback, gedruckt wird danach nur
 * noch der schwanz, der auf einen bildschirm passt. */
void draw_content_reset(void);

/* tests: ausgabe in einen FILE* umlenken statt stdout */
void draw_set_out(FILE *out);

/* nutzbare textbreite einer eingabezeile bei dieser terminalbreite:
 * ohne das " > "-praefix und ohne die spalte, in der der cursor-
 * block am zeilenende sitzt. keys.c braucht dieselbe zahl wie der
 * dock, damit tippen und zeichnen denselben umbruch sehen. */
int input_field_width(int cols);

#endif