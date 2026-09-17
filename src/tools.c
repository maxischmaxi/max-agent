#define _POSIX_C_SOURCE 200809L // NOLINT(bugprone-reserved-identifier)
/* popen, pclose, WEXITSTATUS */

#include "tools.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "cJSON.h"
#include "debug.h"
#include "utils.h"

/* ------------------------------------------------------------------ */
/* definitionen: beschreibungen sind an die models gerichtet –         */
/* praegnend, mit klaren erwartungen an die argumente.                 */
/* ------------------------------------------------------------------ */
static const OaiTool TOOLS[] = {
    {
        .function =
            {
                .name = "read_file",
                .description =
                    "Read the contents of a file at the given path "
                    "(relative to the working directory). Output is "
                    "truncated to 2000 lines or 50KB, whichever is hit "
                    "first; when truncated, a notice at the end tells you "
                    "how to continue. Use offset (1-indexed line number) "
                    "and limit (number of lines) to page through large "
                    "files. When you need the full file, continue with "
                    "offset until complete.",
                .parameters_json =
                    "{\"type\":\"object\",\"properties\":{"
                    "\"path\":{\"type\":\"string\",\"description\":"
                    "\"path of the file to read\"},"
                    "\"offset\":{\"type\":\"number\",\"description\":"
                    "\"line number to start reading from "
                    "(1-indexed)\"},"
                    "\"limit\":{\"type\":\"number\",\"description\":"
                    "\"maximum number of lines to read\"}},"
                    "\"required\":[\"path\"]}",
            },
    },
    {
        .function =
            {
                .name = "edit_file",
                .description =
                    "Edit an existing file with exact text "
                    "replacement. Each edits[].oldText must match the "
                    "file exactly (including whitespace and newlines), "
                    "must occur exactly once and must not overlap with "
                    "other edits. Keep oldText as small as possible while "
                    "still unique - do not include large unchanged regions. "
                    "Prefer this over write_file for targeted changes; use "
                    "write_file only for new files or complete rewrites.",
                .parameters_json =
                    "{\"type\":\"object\",\"properties\":{"
                    "\"path\":{\"type\":\"string\",\"description\":"
                    "\"path of the file to edit\"},"
                    "\"edits\":{\"type\":\"array\",\"minItems\":1,"
                    "\"items\":{\"type\":\"object\","
                    "\"properties\":{"
                    "\"oldText\":{\"type\":\"string\","
                    "\"description\":\"exact text to replace; "
                    "must occur exactly once in the file\"},"
                    "\"newText\":{\"type\":\"string\","
                    "\"description\":\"replacement text "
                    "(empty string deletes the old text)\"}},"
                    "\"required\":[\"oldText\",\"newText\"]}}},"
                    "\"required\":[\"path\",\"edits\"]}",
            },
    },
    {
        .function =
            {
                .name = "write_file",
                .description =
                    "Write content to a file at the given path (relative to "
                    "the working directory). Creates or overwrites the file.",
                .parameters_json =
                    "{\"type\":\"object\",\"properties\":{"
                    "\"path\":{\"type\":\"string\",\"description\":"
                    "\"path of the file to write\"},"
                    "\"content\":{\"type\":\"string\",\"description\":"
                    "\"complete new content of the file\"}},"
                    "\"required\":[\"path\",\"content\"]}",
            },
    },
    {
        .function =
            {
                .name = "bash",
                .description =
                    "Run a shell command in the working directory and return "
                    "its combined output (stdout and stderr) plus the exit "
                    "code. Use this for listing directories, searching, "
                    "building and running tests. Output is truncated to the "
                    "LAST 2000 lines or 50KB (errors and results are at the "
                    "end); when truncated, the full output is saved to a "
                    "temp file whose path is included. Optionally provide a "
                    "timeout in seconds - the command's process group is "
                    "killed when it expires.",
                .parameters_json =
                    "{\"type\":\"object\",\"properties\":{"
                    "\"command\":{\"type\":\"string\",\"description\":"
                    "\"the shell command to run\"},"
                    "\"timeout\":{\"type\":\"number\",\"description\":"
                    "\"timeout in seconds (optional, no default "
                    "timeout)\"}},"
                    "\"required\":[\"command\"]}",
            },
    },
};
#define TOOL_COUNT ((size_t)(sizeof TOOLS / sizeof TOOLS[0]))

const OaiTool *tool_registry(size_t *len)
{
    if (len != NULL) {
        *len = TOOL_COUNT;
    }
    return TOOLS;
}

const OaiTool *tool_find(const char *name)
{
    if (name == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < TOOL_COUNT; i++) {
        if (strcmp(TOOLS[i].function.name, name) == 0) {
            return &TOOLS[i];
        }
    }
    return NULL;
}

bool tool_is_sequential(const char *name)
{
    /* datei-mutationen duerfen einander nicht in die quere kommen:
     * zwei gleichzeitige edit_file/write_file auf dieselbe datei
     * waeren ein lese-schreib-rennen. alles andere (read_file,
     * bash) ist nebenlaeufig harmlos */
    return (bool)(name != NULL && (strcmp(name, "write_file") == 0 ||
                                   strcmp(name, "edit_file") == 0));
}

/* ------------------------------------------------------------------ */
/* hilfsfunktionen                                                      */
/* ------------------------------------------------------------------ */

/* formatierter fehler- bzw. ergebnis-string (heap) */
static char *fmt(const char *format, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, format);
    (void)vsnprintf(buf, sizeof buf, format, ap);
    va_end(ap);
    return dup_str(buf);
}

/* string-argument aus den geparsten json-argumenten holen.
 * fehlt oder kein string: NULL (die meldung baut der aufrufer) */
static char *arg_string(const cJSON *args, const char *key)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(args, key);
    if (!cJSON_IsString(v) || v->valuestring == NULL) {
        return NULL;
    }
    return dup_str(v->valuestring);
}

/* zahl-argument holen: 0, wenn der key fehlt; der wert selbst
 * (auch negativ), wenn er da ist. ist der key vorhanden, aber kein
 * zahl-typ, liefert die funktion -1 (falsch von "wirklich -1"
 * kaum zu unterscheiden – beide faelle sind fuer offset/limit
 * und timeout ungueltig, der aufrufer prueft < 1). */
static long arg_number(const cJSON *args, const char *key)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(args, key);
    if (v == NULL) {
        return 0;
    }
    if (!cJSON_IsNumber(v)) {
        return -1;
    }
    return (long)v->valuedouble;
}

/* ------------------------------------------------------------------ */
/* die vier tools                                                       */
/* ------------------------------------------------------------------ */

static char *tool_read_file(const cJSON *args)
{
    char *path = arg_string(args, "path");
    if (path == NULL) {
        return fmt("error: missing string argument 'path'");
    }

    /* offset: 1-indizierte startzeile, limit: maximale zeilenzahl.
     * 0 = nicht angegeben; negativ (oder kein zahl-typ) = fehler */
    long offset = arg_number(args, "offset");
    long limit = arg_number(args, "limit");
    if (offset < 0 || limit < 0) {
        free(path);
        return fmt("error: 'offset' and 'limit' must be positive "
                   "(offset is a 1-indexed line number)");
    }

    char *buf = NULL;
    size_t size = 0;
    if (read_file(path, &buf, &size) != 0) {
        char *err = fmt("error: cannot read '%s'", path);
        free(path);
        return err;
    }

    /* leere datei: leerer text, kein paging noetig */
    if (size == 0) {
        free(buf);
        free(path);
        return dup_str("");
    }

    /* zeilen-index: anfangs-offset je zeile; die letzte zeile endet
     * bei size (mit oder ohne newline). offs[i]..offs[i+1) ist die
     * i-te zeile (0-indexiert, inklusive ihrem '\n') */
    size_t nl = 0;
    for (size_t i = 0; i < size; i++) {
        if (buf[i] == '\n') {
            nl++;
        }
    }
    size_t total_lines = nl + (buf[size - 1] != '\n' ? 1 : 0);
    size_t *offs = malloc((total_lines + 1) * sizeof *offs);
    if (offs == NULL) {
        die("out of memory");
    }
    offs[0] = 0;
    offs[total_lines] = size;
    {
        size_t li = 1;
        for (size_t i = 0; i < size && li < total_lines; i++) {
            if (buf[i] == '\n') {
                offs[li++] = i + 1;
            }
        }
    }

    char *err = NULL;
    size_t start = (offset > 0) ? (size_t)offset - 1 : 0;
    if (start >= total_lines) {
        err = fmt("error: offset %ld is beyond end of file "
                  "(%zu lines total)",
                  offset, total_lines);
        goto out;
    }

    /* gewaehltes fenster [start, end): ohne limit bis dateiende */
    size_t end = total_lines;
    if (limit > 0) {
        size_t want = start + (size_t)limit;
        end = (want < total_lines) ? want : total_lines;
    }

    /* head-truncation wie der pi-agent: hoechstens TOOL_MAX_LINES
     * zeilen und TOOL_MAX_BYTES bytes, nie mitten in einer zeile.
     * die erste zeile allein zu gross -> bash-fallback statt still */
    size_t shown_end = end;
    if (end - start > TOOL_MAX_LINES) {
        shown_end = start + TOOL_MAX_LINES;
    }
    bool by_bytes = false;
    {
        size_t acc = 0;
        for (size_t i = start; i < shown_end; i++) {
            /* i < shown_end <= end <= total_lines, also ist
             * offs[i+1] immer innerhalb des index – der analyzer kann
             * die kette nicht nachvollziehen (false positive) */
            // NOLINTNEXTLINE(clang-analyzer-security.ArrayBound)
            size_t span = offs[i + 1] - offs[i];
            if (acc + span > TOOL_MAX_BYTES) {
                if (i == start) {
                    err = fmt("line %zu is larger than the 50KB limit. "
                              "Use bash: sed -n '%zup' '%s' "
                              "| head -c 51200",
                              start + 1, start + 1, path);
                    goto out;
                }
                shown_end = i;
                by_bytes = true;
                break;
            }
            acc += span;
        }
    }

    /* text + weiterlese-hinweis. drei faelle:
     *  - truncation: gezeigt [start, shown_end) < gewaehlt [start, end)
     *  - limit stoppte frueher als das dateiende: rest ankuendigen
     *  - am dateiende angekommen: kein hinweis */
    size_t lo = offs[start];
    /* shown_end <= end <= total_lines, also ist offs[shown_end]
     * immer innerhalb des index – der analyzer kann die kette
     * nicht nachvollziehen (false positive) */
    // NOLINTNEXTLINE(clang-analyzer-security.ArrayBound)
    size_t hi = offs[shown_end];
    const char *limit_note = "";
    if (by_bytes) {
        limit_note = " (50KB limit)";
    }
    char hint[208];
    hint[0] = '\0';
    if (shown_end < end) {
        (void)snprintf(hint, sizeof hint,
                       "\n\n[Showing lines %zu-%zu of %zu%s. "
                       "Use offset=%zu to continue.]",
                       start + 1, shown_end, total_lines, limit_note,
                       shown_end + 1);
    } else if (end < total_lines) {
        (void)snprintf(hint, sizeof hint,
                       "\n\n[%zu more lines in file. "
                       "Use offset=%zu to continue.]",
                       total_lines - end, end + 1);
    }

    size_t body = hi - lo;
    char *out = malloc(body + strlen(hint) + 1);
    if (out == NULL) {
        die("out of memory");
    }
    memcpy(out, buf + lo, body);
    memcpy(out + body, hint, strlen(hint) + 1);
    free(offs);
    free(buf);
    free(path);
    return out;

out:
    free(offs);
    free(buf);
    free(path);
    return err;
}

static char *tool_write_file(const cJSON *args)
{
    char *path = arg_string(args, "path");
    char *content = arg_string(args, "content");
    if (path == NULL || content == NULL) {
        free(path);
        free(content);
        return fmt("error: need string arguments 'path' and 'content'");
    }
    size_t len = strlen(content);
    int rc = write_file(path, content, len);
    free(path);
    free(content);
    if (rc != 0) {
        return fmt("error: cannot write file");
    }
    return fmt("ok (%d bytes written)", (int)len);
}

/* ------------------------------------------------------------------ */
/* edit_file: exakte text-ersetzungen statt kompletter neu-auflage   */
/* ------------------------------------------------------------------ */

/* ein geplanter ersatz: bereich [start, start+len) im original plus
 * neuer text. alle positionen zeigen auf DIESELBE original-kopie –
 * ersatz wird nicht inkrementell angewendet (wie beim pi-agenten:
 * jedes oldText matcht das unveraenderte original). */
typedef struct {
    size_t start;
    size_t len;
    char *new_text;
    size_t idx; /* edit-index, fuer fehlermeldungen nach sortierung */
} EditMatch;

/* vorkommen von needle in haystack zaehlen (needle ist hier nie
 * leer – der aufrufer prueft das vorher) */
static size_t count_occurrences(const char *haystack, const char *needle)
{
    size_t n = 0;
    const char *p = haystack;
    size_t len = strlen(needle);
    while ((p = strstr(p, needle)) != NULL) {
        n++;
        p += len;
    }
    return n;
}

/* "\r\n" zu "\n" normalisieren: das modell sieht den inhalt eines
 * CRLF-file ohne \r (chat_append filtert \r aus tool-ergebnissen),
 * also matcht sein oldText nur in LF-form. heap-kopie (owned). */
static char *normalize_lf(const char *s)
{
    size_t len = strlen(s);
    char *out = malloc(len + 1);
    if (out == NULL) {
        die("out of memory");
    }
    size_t j = 0;
    for (size_t i = 0; i < len; i++) {
        if (s[i] == '\r' && i + 1 < len && s[i + 1] == '\n') {
            continue; /* \r vor \n verschwindet */
        }
        out[j++] = s[i];
    }
    out[j] = '\0';
    return out;
}

/* alle "\n" zu "\r\n": rueckwandlung fuer CRLF-dateien beim
 * schreiben – nur aufgerufen, wenn das original CRLF hatte, sonst
 * waere das eine stillschweigende format-aenderung. heap-kopie. */
static char *restore_crlf(const char *s)
{
    size_t len = strlen(s);
    size_t nl = 0;
    for (size_t i = 0; i < len; i++) {
        if (s[i] == '\n') {
            nl++;
        }
    }
    char *out = malloc(len + nl + 1);
    if (out == NULL) {
        die("out of memory");
    }
    size_t j = 0;
    /* j laeuft genau bis len + nl (je newline ein zusaetzliches
     * '\r') – der analyzer kann die schranke nicht nachvollziehen
     * (false positive) */
    // NOLINTBEGIN(clang-analyzer-security.ArrayBound)
    for (size_t i = 0; i < len; i++) {
        if (s[i] == '\n') {
            out[j++] = '\r';
        }
        out[j++] = s[i];
    }
    out[j] = '\0';
    // NOLINTEND(clang-analyzer-security.ArrayBound)
    return out;
}

static char *tool_edit(const cJSON *args)
{
    char *path = arg_string(args, "path");
    if (path == NULL) {
        return fmt("error: missing string argument 'path'");
    }

    /* edits: array aus {oldText, newText}. manche modelle schicken
     * es stattdessen als JSON-string, als einzelnes objekt oder EIN
     * {oldText,newText} direkt auf top-level – alles akzeptieren
     * (der pi-agent toleriert dieselben formen) */
    cJSON *owned_edits = NULL;
    const cJSON *edits = cJSON_GetObjectItemCaseSensitive(args, "edits");
    if (cJSON_IsString(edits) && edits->valuestring != NULL) {
        owned_edits = cJSON_Parse(edits->valuestring);
        if (owned_edits != NULL &&
            (cJSON_IsArray(owned_edits) || cJSON_IsObject(owned_edits))) {
            edits = owned_edits;
        } else {
            cJSON_Delete(owned_edits);
            owned_edits = NULL;
        }
    }
    if (!cJSON_IsArray(edits)) {
        const cJSON *single = cJSON_IsObject(edits) ? edits : args;
        cJSON *old = cJSON_GetObjectItemCaseSensitive(single, "oldText");
        cJSON *nw = cJSON_GetObjectItemCaseSensitive(single, "newText");
        if (cJSON_IsString(old) && cJSON_IsString(nw)) {
            owned_edits = cJSON_CreateArray();
            cJSON *one = cJSON_CreateObject();
            if (owned_edits == NULL || one == NULL ||
                !cJSON_AddItemReferenceToObject(one, "oldText", old) ||
                !cJSON_AddItemReferenceToObject(one, "newText", nw)) {
                die("out of memory");
            }
            cJSON_AddItemToArray(owned_edits, one);
            edits = owned_edits;
        }
    }
    if (!cJSON_IsArray(edits) || cJSON_GetArraySize(edits) == 0) {
        cJSON_Delete(owned_edits);
        free(path);
        return fmt("error: need an 'edits' array with at least one "
                   "{oldText, newText} entry");
    }

    char *buf = NULL;
    size_t size = 0;
    if (read_file(path, &buf, &size) != 0) {
        char *err = fmt("error: cannot read '%s'", path);
        cJSON_Delete(owned_edits);
        free(path);
        return err;
    }

    /* CRLF-datei: in LF-form matchen, beim schreiben zuruech-
     * wandeln – sonst haette der edit die zeilenenden umgeschrieben */
    bool crlf = strstr(buf, "\r\n") != NULL;
    char *content = buf;
    if (crlf) {
        content = normalize_lf(buf);
    }

    /* alle edits gegen das ORIGINAL matchen (nicht inkrementell):
     * fehlt, mehrdeutig oder ueberlappend -> abbruch mit meldung,
     * das modell kann den call korrigieren und erneut schicken */
    size_t n_edits = (size_t)cJSON_GetArraySize(edits);
    EditMatch *m = calloc(n_edits, sizeof *m);
    if (m == NULL) {
        die("out of memory");
    }
    char *err = NULL;
    for (size_t i = 0; i < n_edits && err == NULL; i++) {
        const cJSON *e = cJSON_GetArrayItem(edits, (int)i);
        const cJSON *o = cJSON_GetObjectItemCaseSensitive(e, "oldText");
        const cJSON *n = cJSON_GetObjectItemCaseSensitive(e, "newText");
        if (!cJSON_IsString(o) || o->valuestring == NULL ||
            o->valuestring[0] == '\0') {
            err = fmt("error: edits[%zu].oldText must be a non-empty "
                      "string",
                      i);
            break;
        }
        if (!cJSON_IsString(n) || n->valuestring == NULL) {
            err = fmt("error: edits[%zu].newText must be a string", i);
            break;
        }
        char *old = normalize_lf(o->valuestring);
        const char *hit = strstr(content, old);
        if (hit == NULL) {
            err = fmt("error: could not find edits[%zu] in '%s'. the "
                      "oldText must match exactly including all "
                      "whitespace and newlines",
                      i, path);
        } else if (count_occurrences(content, old) > 1) {
            err = fmt("error: found %zu occurrences of edits[%zu] in '%s'. "
                      "each oldText must be unique - provide more context "
                      "to make it unique",
                      count_occurrences(content, old), i, path);
        } else {
            m[i].start = (size_t)(hit - content);
            m[i].len = strlen(old);
            m[i].new_text = normalize_lf(n->valuestring);
            m[i].idx = i;
        }
        free(old);
    }
    if (err != NULL) {
        goto fail;
    }

    /* nach position sortieren (insertion-sort, klein und stabil),
     * dann ueberlappungen erkennen – sortierung noetig, weil die
     * reihenfolge der edits beliebiger text-positionen folgt */
    for (size_t i = 1; i < n_edits; i++) {
        EditMatch key = m[i];
        size_t j = i;
        while (j > 0 && m[j - 1].start > key.start) {
            m[j] = m[j - 1];
            j--;
        }
        m[j] = key;
    }
    for (size_t i = 1; i < n_edits; i++) {
        if (m[i - 1].start + m[i - 1].len > m[i].start) {
            err = fmt("error: edits[%zu] and edits[%zu] overlap in '%s'. "
                      "merge them into one edit or target disjoint "
                      "regions",
                      m[i - 1].idx, m[i].idx, path);
            goto fail;
        }
    }

    /* ergebnis zusammenkopieren: segmente zwischen den treffern
     * bleiben unangetastet, jeder treffer wird durch newText ersetzt */
    size_t content_len = strlen(content);
    long long extra = 0;
    for (size_t i = 0; i < n_edits; i++) {
        /* new_text ist hier nie NULL: nur erfolgreiche matches
         * kommen bis hier (false positive des analyzers) */
        // NOLINTNEXTLINE(clang-analyzer-core.NonNullParamChecker)
        extra += (long long)strlen(m[i].new_text) - (long long)m[i].len;
    }
    char *out = malloc(content_len + (size_t)extra + 1);
    if (out == NULL) {
        die("out of memory");
    }
    size_t pos = 0;
    size_t w = 0;
    for (size_t i = 0; i < n_edits; i++) {
        memcpy(out + w, content + pos, m[i].start - pos);
        w += m[i].start - pos;
        size_t new_len = strlen(m[i].new_text);
        memcpy(out + w, m[i].new_text, new_len);
        w += new_len;
        pos = m[i].start + m[i].len;
    }
    memcpy(out + w, content + pos, content_len - pos);
    w += content_len - pos;
    out[w] = '\0';

    if (w == content_len && memcmp(out, content, w) == 0) {
        err = fmt("error: no changes made to '%s'. the replacements "
                  "produced identical content",
                  path);
        free(out);
        goto fail;
    }

    char *final = out;
    if (crlf) {
        final = restore_crlf(out);
        free(out);
    }
    int rc = write_file(path, final, strlen(final));
    free(final);
    for (size_t i = 0; i < n_edits; i++) {
        free(m[i].new_text);
    }
    free(m);
    if (content != buf) {
        free(content);
    }
    free(buf);
    cJSON_Delete(owned_edits);
    if (rc != 0) {
        char *werr = fmt("error: cannot write file '%s'", path);
        free(path);
        return werr;
    }
    char *ok = fmt("ok: replaced %zu block(s) in '%s'", n_edits, path);
    free(path);
    return ok;

fail:
    for (size_t i = 0; i < n_edits; i++) {
        free(m[i].new_text);
    }
    free(m);
    if (content != buf) {
        free(content);
    }
    free(buf);
    cJSON_Delete(owned_edits);
    free(path);
    return err;
}

/* anfang (byte-offset) des bash-tail-fensters: hoechstens
 * TOOL_MAX_LINES zeilen und TOOL_MAX_BYTES bytes, an zeilen-grenzen
 * ausgerichtet. sonderfall: die letzte zeile ist selbst laenger
 * als das byte-limit -> nur ihr ende (an utf-8-grenze), denn
 * weiter hinten kann keine zeile mehr anfangen, die ins fenster
 * gepasst haette. */
static size_t tail_start(const char *s, size_t len)
{
    if (len == 0) {
        return 0;
    }
    /* letzte newline suchen – hoechstens TOOL_MAX_BYTES weit */
    size_t scan = (len > TOOL_MAX_BYTES) ? len - TOOL_MAX_BYTES : 0;
    size_t last_nl = len;
    for (size_t i = len; i > scan; i--) {
        if (s[i - 1] == '\n') {
            last_nl = i - 1;
            break;
        }
    }
    if (last_nl == len) {
        /* keine newline im fensterbereich: die letzte zeile ist
         * zu lang -> nur ihr ende behalten */
        size_t lo = scan;
        while (lo < len && (s[lo] & 0xC0) == 0x80) {
            lo++; /* utf-8-fortsetzungsbyte: grenze nach vorne */
        }
        return lo;
    }
    /* fenster von hinten aufbauen. endet der output auf '\n', ist
     * [last_nl+1, len) LEER – die letzte inhaltliche zeile kommt
     * erst in der schleife. endet er offen, liegt sie schon drin. */
    size_t lo;
    size_t lines;
    if (s[len - 1] == '\n') {
        lo = len;
        lines = 0;
    } else {
        lo = last_nl + 1;
        lines = 1;
    }
    while (lo > 0 && lines < TOOL_MAX_LINES) {
        size_t start = lo - 1; /* die zeile davor endet hier */
        while (start > 0 && s[start - 1] != '\n') {
            start--;
        }
        if (len - start > TOOL_MAX_BYTES) {
            break; /* diese zeile passte nicht mehr ganz */
        }
        lo = start;
        lines++;
    }
    return lo;
}

/* laufende bash-kinder. seit die tool-calls einer runde PARALLEL
 * laufen, koennen mehrere gleichzeitig existieren – jedes mit
 * eigener prozessgruppe. eintrag nach dem fork (worker-thread),
 * austrag nach dem wait; das killen im abbruch-fall (haupt-thread)
 * erwischt ALLE. atomar, weil threads. 0 = slot frei. mehr slots
 * als tool-calls pro runde (SEND_MAX_TOOLS = 32) gibt es nicht. */
#define BASH_KID_SLOTS 64
static atomic_int g_bash_kids[BASH_KID_SLOTS];

static void bash_kid_add(pid_t pid)
{
    for (size_t i = 0; i < BASH_KID_SLOTS; i++) {
        int free = 0;
        if (atomic_compare_exchange_strong(&g_bash_kids[i], &free, (int)pid)) {
            return;
        }
    }
    /* alle slots belegt: ohne kill-schutz laufen – der aufrufer
     * (agent-loop) deckt die anzahl ohnehin auf 32 */
    dbg("bash: kid-registrierung voll (pid=%ld)", (long)pid);
}

static void bash_kid_remove(pid_t pid)
{
    for (size_t i = 0; i < BASH_KID_SLOTS; i++) {
        int slot = atomic_load(&g_bash_kids[i]);
        if (slot == (int)pid) {
            atomic_store(&g_bash_kids[i], 0);
            return;
        }
    }
}

/* die laufenden tool-calls abbrechen (haupt-thread, bei ctrl+c/esc):
 * SIGKILL an die PROZESSGRUPPE aller registrierten kinder – das
 * erwischt die shells samt aller von ihnen gestarteten kinder. die
 * worker-threads kehren danach sofort aus dem read zurueck (EOF). */
void tool_kill_current(void)
{
    for (size_t i = 0; i < BASH_KID_SLOTS; i++) {
        int child = atomic_load(&g_bash_kids[i]);
        if (child > 0) {
            dbg("bash: kill prozessgruppe %d", child);
            (void)kill(-child, SIGKILL);
            (void)kill(child, SIGKILL);
        }
    }
}

static char *tool_bash(const cJSON *args)
{
    char *command = arg_string(args, "command");
    if (command == NULL) {
        return fmt("error: missing string argument 'command'");
    }
    /* timeout: sekunden, optional (0 = keiner). beim ablauf wird die
     * prozessgruppe gekillt; der output bis dahin zaehlt noch mit */
    long timeout_s = arg_number(args, "timeout");
    if (timeout_s < 0 || timeout_s > 86400) {
        free(command);
        return fmt("error: 'timeout' must be a number of seconds "
                   "between 1 and 86400");
    }
    /* stderr wird mit eingesammelt; die subshell-klammer stellt
     * sicher, dass NUTZER-redirections (z.B. "echo x 1>&2")
     * zuerst wirken und "2>&1" sie nicht kaputtmacht */
    size_t clen = strlen(command);
    /* "(%s) 2>&1": klammer, space und "2>&1" sind 7 zeichen + NUL */
    size_t cmd_len = clen + 8;
    char *cmd_buf = malloc(cmd_len);
    if (cmd_buf == NULL) {
        free(command);
        return NULL;
    }
    (void)snprintf(cmd_buf, cmd_len, "(%s) 2>&1", command);
    free(command);

    /* eigenes fork/exec statt popen: popen gibt die pid nicht her,
     * und wir MUessen das kind beim abbruch killen koennen. das
     * kind bekommt eine eigene prozessgruppe (kill -pid erwischt
     * auch seine kinder) und stdin aus /dev/null – ein kommando,
     * das eingaben erwartet, kann das terminal NIE anfassen. */
    int outfd[2];
    if (pipe(outfd) != 0) {
        free(cmd_buf);
        return fmt("error: pipe failed");
    }
    // NOLINTNEXTLINE(bugprone-command-processor)
    pid_t pid = fork();
    if (pid < 0) {
        close(outfd[0]);
        close(outfd[1]);
        free(cmd_buf);
        return fmt("error: fork failed");
    }
    if (pid == 0) {
        /* kind: eigene prozessgruppe, umlenken, exec */
        (void)setpgid(0, 0);
        int devnull = open("/dev/null", O_RDONLY);
        if (devnull >= 0) {
            (void)dup2(devnull, STDIN_FILENO);
            close(devnull);
        }
        (void)dup2(outfd[1], STDOUT_FILENO);
        (void)dup2(outfd[1], STDERR_FILENO);
        close(outfd[0]);
        close(outfd[1]);
        execl("/bin/sh", "sh", "-c", cmd_buf, (char *)NULL);
        _exit(127);
    }
    free(cmd_buf);
    close(outfd[1]);
    bash_kid_add(pid);
    dbg("bash: fork pid=%ld", (long)pid);

    /* output einsammeln bis EOF, hoechstens TOOL_BASH_COLLECT_MAX.
     * rohes read auf dem pipe-fd (kein stdio): der leser ist der
     * worker-thread, und beim kill des kinds (abbruch oder timeout)
     * liefert read sofort EOF. poll statt blocking read: nur so
     * laesst sich das timeout ueberwachen. jenseits des sammel-
     * limits wird weitergelesen und verworfen – das kind darf nie
     * auf einem vollen pipe-buffer haengen bleiben */
    size_t cap = 4096;
    size_t got = 0;
    char *buf = malloc(cap);
    if (buf == NULL) {
        close(outfd[0]);
        (void)waitpid(pid, NULL, 0);
        bash_kid_remove(pid);
        return NULL;
    }
    bool capped = false; /* sammel-limit erreicht: rest verwerfen */
    bool timed_out = false;
    long long deadline = (timeout_s > 0) ? mono_ms() + (timeout_s * 1000) : -1;
    struct pollfd pfd = {.fd = outfd[0], .events = POLLIN};

    for (;;) {
        int wait_ms = -1;
        if (deadline > 0 && !timed_out) {
            long long remain = deadline - mono_ms();
            if (remain <= 0) {
                timed_out = true;
                dbg("bash: timeout – kill prozessgruppe %ld", (long)pid);
                (void)kill(-pid, SIGKILL);
                continue;
            }
            wait_ms = (remain > 60000) ? 60000 : (int)remain;
        }
        int pr = poll(&pfd, 1, wait_ms);
        if (pr < 0) {
            if (errno == EINTR) {
                continue; /* signal: einfach weiterpollen */
            }
            break; /* fehler: was da ist, ist da */
        }
        if (pr == 0) {
            timed_out = true;
            dbg("bash: timeout – kill prozessgruppe %ld", (long)pid);
            (void)kill(-pid, SIGKILL);
            continue;
        }
        /* puffer voll: wachsen bis sammel-limit, danach verwerfen */
        if (!capped && got + 1 >= cap) {
            if (cap < TOOL_BASH_COLLECT_MAX) {
                size_t ncap = cap * 2;
                if (ncap > TOOL_BASH_COLLECT_MAX) {
                    ncap = TOOL_BASH_COLLECT_MAX;
                }
                char *grown = realloc(buf, ncap);
                if (grown == NULL) {
                    free(buf);
                    close(outfd[0]);
                    (void)waitpid(pid, NULL, 0);
                    bash_kid_remove(pid);
                    return NULL;
                }
                buf = grown;
                cap = ncap;
            } else {
                capped = true; /* genug gesammelt: rest verwerfen */
            }
        }
        char scratch[4096];
        char *dst = scratch;
        size_t room = sizeof scratch;
        if (!capped) {
            dst = buf + got;
            room = cap - got - 1;
        }
        if (room == 0) {
            capped = true;
            continue;
        }
        ssize_t chunk = read(outfd[0], dst, room);
        if (chunk < 0) {
            if (errno == EINTR) {
                continue; /* signal: einfach weiterlesen */
            }
            break; /* lesefehler: was da ist, ist da */
        }
        if (chunk == 0) {
            break; /* eof: das kind ist fertig (oder gekillt) */
        }
        if (!capped) {
            got += (size_t)chunk;
        }
    }
    buf[got] = '\0'; /* terminierung EINMAL nach der schleife */
    close(outfd[0]);
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
        /* weiter warten: nur EINTR abfangen */
    }
    bash_kid_remove(pid);
    int exit_code = -1;
    if (WIFEXITED(status)) {
        exit_code = WEXITSTATUS(status);
    }

    /* zeilen zaehlen (fuer den truncation-hinweis) */
    size_t total_lines = 0;
    for (size_t i = 0; i < got; i++) {
        if (buf[i] == '\n') {
            total_lines++;
        }
    }
    if (got > 0 && buf[got - 1] != '\n') {
        total_lines++;
    }

    bool truncated =
        (bool)(capped || got > TOOL_MAX_BYTES || total_lines > TOOL_MAX_LINES);

    if (!truncated) {
        /* kleine outputs: unverkuertztes ergebnis, nur statuszeilen */
        char *out = realloc(buf, got + 64);
        if (out == NULL) {
            free(buf);
            return NULL;
        }
        size_t pos = got;
        if (timed_out) {
            pos += (size_t)snprintf(out + pos, 48, "\n[timeout after %lds]",
                                    timeout_s);
        }
        (void)snprintf(out + pos, 32, "\n[exit: %d]", exit_code);
        return out;
    }

    /* vollstaendigen output (bis sammel-limit) in eine temp-datei:
     * das model kann sie gezielt greppen/seden, statt das kommando
     * mit tail-varianten erneut laufen zu lassen */
    char tmp_path[] = "/tmp/max-agent-bash-XXXXXX";
    int tfd = mkstemp(tmp_path);
    bool tmp_ok = tfd >= 0;
    if (tmp_ok) {
        size_t off = 0;
        while (off < got) {
            ssize_t n = write(tfd, buf + off, got - off);
            if (n < 0) {
                if (errno == EINTR) {
                    continue;
                }
                tmp_ok = false;
                break;
            }
            off += (size_t)n;
        }
        close(tfd);
    }
    if (!tmp_ok) {
        dbg("bash: temp-datei fehlgeschlagen (%s)", strerror(errno));
    }

    /* tail-fenster: die LETZTEN zeilen – fehler und ergebnisse
     * stehen am output-ende, nicht am anfang */
    size_t lo = tail_start(buf, got);
    size_t lines_shown = 0;
    for (size_t i = lo; i < got; i++) {
        if (buf[i] == '\n') {
            lines_shown++;
        }
    }
    if (got > 0 && buf[got - 1] != '\n') {
        lines_shown++;
    }
    bool partial = (bool)(lo > 0 && buf[lo - 1] != '\n');

    const char *full = "(temp file could not be written)";
    if (tmp_ok) {
        full = tmp_path;
    }
    char hint[352];
    if (partial) {
        /* fenster beginnt mitten in einer zu langen zeile */
        (void)snprintf(hint, sizeof hint,
                       "\n\n[Showing the last 50KB of line %zu. "
                       "Full output: %s]",
                       total_lines, full);
    } else if (capped) {
        (void)snprintf(hint, sizeof hint,
                       "\n\n[Showing lines %zu-%zu of at least %zu "
                       "(collection capped at 16MB). Full output: %s]",
                       total_lines - lines_shown + 1, total_lines, total_lines,
                       full);
    } else {
        (void)snprintf(hint, sizeof hint,
                       "\n\n[Showing lines %zu-%zu of %zu. "
                       "Full output: %s]",
                       total_lines - lines_shown + 1, total_lines, total_lines,
                       full);
    }

    size_t body = got - lo;
    size_t hint_len = strlen(hint);
    char *out = malloc(body + hint_len + 64);
    if (out == NULL) {
        free(buf);
        return NULL;
    }
    memcpy(out, buf + lo, body);
    memcpy(out + body, hint, hint_len + 1);
    size_t pos = body + hint_len;
    if (timed_out) {
        pos += (size_t)snprintf(out + pos, 48, "\n[timeout after %lds]",
                                timeout_s);
    }
    /* exit-code hinten dran, format "[exit: N]" – die eckigen
     * klammern markieren den code als maschinenlesbares feld; die
     * ANZEIGE streicht sie (draw.c faerbt "exit: N" gruen/rot, der
     * wrap-pass ueberliest die leerzeile davor) */
    (void)snprintf(out + pos, 32, "\n[exit: %d]", exit_code);
    free(buf);
    return out;
}

/* ------------------------------------------------------------------ */
/* public api                                                          */
/* ------------------------------------------------------------------ */
char *tool_execute(const char *name, const char *arguments_json)
{
    if (name == NULL || arguments_json == NULL) {
        return fmt("error: tool name or arguments missing");
    }
    if (tool_find(name) == NULL) {
        return fmt("error: unknown tool '%s'", name);
    }

    cJSON *args = cJSON_Parse(arguments_json);
    if (args == NULL) {
        return fmt("error: arguments are not valid json");
    }

    char *result = NULL;
    if (strcmp(name, "read_file") == 0) {
        result = tool_read_file(args);
    } else if (strcmp(name, "write_file") == 0) {
        result = tool_write_file(args);
    } else if (strcmp(name, "edit_file") == 0) {
        result = tool_edit(args);
    } else if (strcmp(name, "bash") == 0) {
        result = tool_bash(args);
    } else {
        result = fmt("error: unknown tool '%s'", name);
    }
    cJSON_Delete(args);

    if (result == NULL) {
        return fmt("error: tool failed");
    }
    return result;
}