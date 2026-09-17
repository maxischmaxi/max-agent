#define _POSIX_C_SOURCE 200809L // NOLINT(bugprone-reserved-identifier)

#include "history.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "utils.h"

/* ------------------------------------------------------------------ */
/* persistenz: ~/.config/.maxagent/history, eine eingabe pro zeile.   */
/* mehrzeilige eingaben werden mit "\\n" statt echtem umbruch kodiert  */
/* (NUL/newline waeren unlesbar), beim laden zurueckgewandelt.        */
/* ------------------------------------------------------------------ */

static char *history_path(void)
{
    return append_to_home(".config/.maxagent/history");
}

/* "\\n" -> '\n' dekodieren. der string enthaelt nie eine NUL, also
 * reicht laenge-mischen mit memmove. */
static char *decode_escapes(const char *s, size_t n)
{
    char *out = malloc(n + 1);
    if (out == NULL) {
        return NULL;
    }
    size_t w = 0;
    for (size_t r = 0; r < n;) {
        if (r + 1 < n && s[r] == '\\' && s[r + 1] == 'n') {
            out[w++] = '\n';
            r += 2;
        } else {
            out[w++] = s[r++];
        }
    }
    out[w] = '\0';
    return out;
}

/* '\n' -> "\\n" kodieren (andere backslashes bleiben, wie sie sind:
 * dekodiert wird nur die 2-zeichen-folge "\\n", ein doppel-backslash
 * davor wuerde sie nicht ungeschehen machen – praktisch tippt
 * niemand "\\n" am zeilenanfang, und falsch dekodiert waere nur die
 * anzeige, nicht die datei). */
static size_t encoded_len(const char *s)
{
    size_t n = 0;
    for (const char *p = s; *p != '\0'; p++) {
        n += (*p == '\n') ? 2 : 1;
    }
    return n;
}

static char *encode_escapes(const char *s)
{
    size_t len = encoded_len(s);
    char *out = malloc(len + 1);
    if (out == NULL) {
        return NULL;
    }
    size_t w = 0;
    for (const char *p = s; *p != '\0'; p++) {
        if (*p == '\n') {
            out[w++] = '\\';
            out[w++] = 'n';
        } else {
            out[w++] = *p;
        }
    }
    out[w] = '\0';
    return out;
}

void history_load(History *h)
{
    if (h == NULL) {
        return;
    }
    history_free(h);

    char *path = history_path();
    if (path == NULL) {
        return;
    }
    FILE *f = fopen(path, "r");
    free(path);
    if (f == NULL) {
        return; /* noch keine history: leer anfangen */
    }

    char *line = NULL;
    size_t cap = 0;
    ssize_t n;
    while ((n = getline(&line, &cap, f)) != -1) {
        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) {
            n--; /* zeilenende weg */
        }
        if (n == 0) {
            continue;
        }
        if ((size_t)n > HISTORY_LINE_MAX) {
            continue; /* muell/zusammengeklebte zeile: weg */
        }
        char *entry = decode_escapes(line, (size_t)n);
        if (entry == NULL) {
            continue;
        }
        /* deduplizieren: eine zeile, die schon der juengste eintrag
         * ist, nicht nochmal aufnehmen */
        if (h->len == 0 || strcmp(h->entries[h->len - 1], entry) != 0) {
            if (h->len == HISTORY_MAX) {
                free(h->entries[0]); /* aeltesten verdraengen */
                memmove((void *)h->entries, (const void *)(h->entries + 1),
                        (HISTORY_MAX - 1) * sizeof *h->entries);
                h->len--;
            }
            h->entries[h->len++] = entry;
        } else {
            free(entry);
        }
    }
    free(line);
    (void)fclose(f);
}

/* die eingabe als EINE zeile ans file haengen. best effort: ein
 * fehler verliert diese eingabe, nicht die app. */
static void history_persist(const char *text)
{
    char *encoded = encode_escapes(text);
    if (encoded == NULL) {
        return;
    }
    if (strlen(encoded) > HISTORY_LINE_MAX) {
        free(encoded);
        return; /* laenger als jede zeile, die wir lesen */
    }
    char *path = history_path();
    if (path == NULL) {
        free(encoded);
        return;
    }
    /* verzeichnis anlegen (idempotent): frisches home, erste
     * eingabe ueberhaupt. scheitert das, scheitert unten das
     * fopen und die eingabe geht nur auf platte verloren. */
    char *dir = append_to_home(".config/.maxagent");
    if (dir != NULL) {
        (void)mkdir_p(dir, 0755);
        free(dir);
    }
    FILE *f = fopen(path, "a");
    if (f != NULL) {
        size_t n = strlen(encoded);
        if (fwrite(encoded, 1, n, f) == n) {
            (void)fputc('\n', f);
        }
        (void)fclose(f);
    }
    free(path);
    free(encoded);
}

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

    history_persist(text);
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