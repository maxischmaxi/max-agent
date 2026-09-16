#ifndef MAX_AGENT_HISTORY
#define MAX_AGENT_HISTORY

#include <stddef.h>

/* ------------------------------------------------------------------ */
/* history: die zuletzt abgeschickten eingaben, zum zurueckholen mit  */
/* pfeil-hoch (wie in einer shell). gilt fuer die laufende sitzung –  */
/* nichts wird auf platte geschrieben.                                */
/*                                                                    */
/* das blaettern merkt sich den text, der beim ersten pfeil-hoch im   */
/* feld stand: wer sich verirrt, kommt mit pfeil-runter zu seinem     */
/* entwurf zurueck.                                                   */
/* ------------------------------------------------------------------ */

/* mehr eintraege braucht in einer sitzung niemand, und der ring
 * bleibt damit ueberschaubar (kein wachsendes array) */
#define HISTORY_MAX 64

typedef struct {
    char *entries[HISTORY_MAX]; /* [0] aeltester ... [len-1] juengster */
    size_t len;
    /* blaetter-position: 0 = nicht am blaettern, 1 = juengster
     * eintrag, len = aeltester */
    size_t pos;
    char *draft; /* eingabe vor dem ersten pfeil-hoch, NULL = leer */
} History;

/* eine abgeschickte eingabe aufnehmen. leere eingaben und direkte
 * wiederholungen des juengsten eintrags werden ignoriert (sonst
 * steht dieselbe zeile mehrfach im weg). ist der ring voll, faellt
 * der aelteste eintrag raus. beendet ein laufendes blaettern. */
void history_add(History *h, const char *text);

/* einen eintrag zurueck (aelter). `current` ist der gerade getippte
 * text; beim ersten schritt wird er als entwurf gemerkt (NULL =
 * leeres feld). rueckgabe ist der einzusetzende text, oder NULL,
 * wenn es nichts aelteres gibt – dann bleibt die eingabe stehen.
 * der zeiger gehoert der history und gilt bis zum naechsten aufruf. */
const char *history_prev(History *h, const char *current);

/* einen eintrag vor (juenger). hinter dem juengsten eintrag kommt
 * der gemerkte entwurf zurueck ("" wenn das feld leer war) und das
 * blaettern endet. NULL = wir blaettern gar nicht, nichts zu tun. */
const char *history_next(History *h);

/* blaettern beenden, ohne die eingabe anzufassen (z.B. bei escape) */
void history_reset(History *h);

/* alle eintraege und den entwurf freigeben */
void history_free(History *h);

#endif
