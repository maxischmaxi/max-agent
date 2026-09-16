#include "history.h"

#include <stdlib.h>
#include <string.h>

#include "utils.h"

void history_reset(History *h)
{
    if (h == NULL) {
        return;
    }
    h->pos = 0;
    free(h->draft);
    h->draft = NULL;
}

void history_add(History *h, const char *text)
{
    if (h == NULL || text == NULL || text[0] == '\0') {
        return;
    }
    history_reset(h); /* jede neue eingabe beendet das blaettern */

    /* direkte wiederholung: der eintrag steht schon oben */
    if (h->len > 0 && strcmp(h->entries[h->len - 1], text) == 0) {
        return;
    }

    char *copy = dup_str(text);
    if (copy == NULL) {
        die("out of memory");
    }
    if (h->len == HISTORY_MAX) {
        free(h->entries[0]); /* aeltesten verdraengen */
        memmove((void *)h->entries, (const void *)(h->entries + 1),
                (HISTORY_MAX - 1) * sizeof *h->entries);
        h->len--;
    }
    h->entries[h->len++] = copy;
}

const char *history_prev(History *h, const char *current)
{
    if (h == NULL || h->len == 0 || h->pos >= h->len) {
        return NULL; /* nichts da, oder schon beim aeltesten */
    }
    if (h->pos == 0) {
        /* erster schritt: den entwurf sichern, damit pfeil-runter
         * ihn spaeter zurueckgeben kann */
        free(h->draft);
        h->draft = NULL;
        if (current != NULL && current[0] != '\0') {
            h->draft = dup_str(current);
            if (h->draft == NULL) {
                die("out of memory");
            }
        }
    }
    h->pos++;
    return h->entries[h->len - h->pos];
}

const char *history_next(History *h)
{
    if (h == NULL || h->pos == 0) {
        return NULL; /* wir blaettern gar nicht */
    }
    h->pos--;
    if (h->pos == 0) {
        /* hinter dem juengsten eintrag: zurueck zum entwurf. "" ist
         * das richtige ergebnis fuer ein vorher leeres feld – NULL
         * hiesse "nichts tun" und wuerde den eintrag stehen lassen. */
        return (h->draft != NULL) ? h->draft : "";
    }
    return h->entries[h->len - h->pos];
}

void history_free(History *h)
{
    if (h == NULL) {
        return;
    }
    for (size_t i = 0; i < h->len; i++) {
        free(h->entries[i]);
    }
    h->len = 0;
    h->pos = 0;
    free(h->draft);
    h->draft = NULL;
}
