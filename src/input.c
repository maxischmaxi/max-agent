#include "input.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "utils.h"

/* zeiger auf den anfang des letzten worts in der letzten zeile.
 * bei leerem wort zeigt er auf den string-anfang ('\0'). */
const char *last_word(const Input *in)
{
    const char *last = in->lines[in->count - 1];
    size_t len = strlen(last);
    size_t start = len;
    /* false positive: der analyzer sieht einen 1-byte-calloc-block
     * mit strlen > 0 (unmoeglich) - block ist immer >= strlen+1 gross */
    // NOLINTBEGIN(clang-analyzer-security.ArrayBound)
    while (start > 0 && last[start - 1] != ' ') {
        start--;
    }
    // NOLINTEND(clang-analyzer-security.ArrayBound)
    return &last[start];
}

bool input_in_cmd(const Input *in)
{
    const char *word = last_word(in);
    return word[0] == '/';
}

void cmd_prefix(const Input *in, char *out, size_t out_sz)
{
    out[0] = '\0';
    const char *word = last_word(in);
    if (word[0] != '/' || out_sz == 0) {
        return;
    }
    size_t plen = strlen(word + 1); /* wort ohne fuehrendes '/' */
    if (plen >= out_sz) {
        plen = out_sz - 1;
    }
    memcpy(out, word + 1, plen);
    out[plen] = '\0';
}

void input_free(Input *in)
{
    for (size_t i = 0; i < in->count; i++) {
        free(in->lines[i]);
    }
    in->count = 0;
    in->cursor = 0;
}

/* laenge der aktuellen zeile (lines[count-1]) */
size_t input_len(const Input *in)
{
    return strlen(in->lines[in->cursor_line]);
}

/* ------------------------------------------------------------------ */
/* cursor-utility: nur bewegung, kein text-aenderung. die invariante   */
/* (cursor <= len) wird von jeder funktion bewahrt.                    */
/* ------------------------------------------------------------------ */

void input_cursor_set(Input *in, size_t pos)
{
    size_t len = input_len(in);
    if (pos > len) {
        pos = len; /* klemmen: nie hinter das zeilenende */
    }
    in->cursor = pos;
}

void input_cursor_line_set(Input *in, size_t line)
{
    if (line >= in->count) {
        line = in->count - 1; /* klemmen: letzte zeile */
    }
    in->cursor_line = line;
    /* byte-cursor an die neue zeile anpassen */
    input_cursor_set(in, in->cursor);
}

void input_cursor_home(Input *in)
{
    in->cursor = 0;
}

void input_cursor_end(Input *in)
{
    in->cursor = input_len(in);
}

/* utf8-zeichenlaengen (definitionen weiter unten bei wrap_step) */
static size_t utf8_at(const char *s, size_t i);
static size_t utf8_before(const char *s, size_t i);

bool input_cursor_left(Input *in)
{
    if (in->cursor == 0) {
        /* zeilenanfang: ueber die grenze in die vorherige zeile */
        if (in->cursor_line == 0) {
            return false; /* anfang des gesamten buffers */
        }
        in->cursor_line--;
        in->cursor = input_len(in); /* ans ende der zeile davor */
        return true;
    }
    /* ganzes utf-8-zeichen: bis zum lead-byte zurueck */
    in->cursor -= utf8_before(in->lines[in->cursor_line], in->cursor);
    return true;
}

bool input_cursor_right(Input *in)
{
    if (in->cursor >= input_len(in)) {
        /* zeilenende: ueber die grenze in die naechste zeile */
        if (in->cursor_line + 1 >= in->count) {
            return false; /* ende des gesamten buffers */
        }
        in->cursor_line++;
        in->cursor = 0;
        return true;
    }
    /* ganzes utf-8-zeichen vorruecken */
    char *line = in->lines[in->cursor_line];
    in->cursor += utf8_at(line, in->cursor);
    return true;
}

/* zeilenumbruch VOR dem cursor loeschen: die aktuelle zeile haengt
 * an die vorherige, der cursor landet an der ehemaligen grenze */
bool input_join_prev(Input *in)
{
    if (in->cursor_line == 0) {
        return false; /* es gibt keine vorherige zeile */
    }
    char *prev = in->lines[in->cursor_line - 1];
    char *cur = in->lines[in->cursor_line];
    size_t prev_len = strlen(prev);
    size_t cur_len = strlen(cur);

    char *grown = realloc(prev, prev_len + cur_len + 1);
    if (!grown) {
        die("out of memory");
    }
    memcpy(grown + prev_len, cur, cur_len + 1); /* incl. '\0' */
    /* gemergte zeile zurueckschreiben, dann erst freigeben */
    in->lines[in->cursor_line - 1] = grown;
    free(cur);

    /* lines[cursor_line] entfaellt: die zeilen dahinter eine
     * position vorruecken */
    memmove((void *)&in->lines[in->cursor_line],
            (const void *)&in->lines[in->cursor_line + 1],
            (in->count - in->cursor_line - 1) * sizeof in->lines[0]);
    in->count--;
    in->cursor_line--;
    in->cursor = prev_len; /* an der jetzt geloeschten grenze */
    return true;
}

/* zeilenumbruch HINTER dem cursor loeschen: die naechste zeile haengt
 * an die aktuelle, der cursor bleibt an seiner position */
bool input_join_next(Input *in)
{
    if (in->cursor_line + 1 >= in->count) {
        return false; /* es gibt keine naechste zeile */
    }
    char *cur = in->lines[in->cursor_line];
    char *next = in->lines[in->cursor_line + 1];
    size_t cur_len = strlen(cur);
    size_t next_len = strlen(next);

    char *grown = realloc(cur, cur_len + next_len + 1);
    if (!grown) {
        die("out of memory");
    }
    memcpy(grown + cur_len, next, next_len + 1); /* incl. '\0' */
    /* gemergte zeile zurueckschreiben, dann erst freigeben */
    in->lines[in->cursor_line] = grown;
    free(next);

    /* lines[cursor_line+1] entfaellt: die zeilen dahinter eine
     * position vorruecken */
    memmove((void *)&in->lines[in->cursor_line + 1],
            (const void *)&in->lines[in->cursor_line + 2],
            (in->count - in->cursor_line - 2) * sizeof in->lines[0]);
    in->count--;
    return true; /* cursor bleibt an seiner position */
}

/* zeichen UNTER dem cursor loeschen; am zeilenende den umbruch */
bool input_delete_forward(Input *in)
{
    char *line = in->lines[in->cursor_line];
    size_t len = strlen(line);
    if (in->cursor < len) {
        /* ganzes utf-8-zeichen loeschen: umlaut/emoji sind mehrere
         * bytes, ein halbes wuerde kaputte zeichen hinterlassen */
        size_t n = utf8_at(line, in->cursor);
        if (in->cursor + n > len) {
            n = 1; /* kaputte folge am rand: nur das byte */
        }
        memmove(line + in->cursor, line + in->cursor + n,
                len - in->cursor - n + 1);
        return true;
    }
    return input_join_next(in); /* am zeilenende: umbruch dahinter */
}

/* von cursor bis zeilenende loeschen; am ende den umbruch dahinter */
bool input_kill_to_end(Input *in)
{
    char *line = in->lines[in->cursor_line];
    size_t len = strlen(line);
    if (in->cursor < len) {
        line[in->cursor] = '\0';
        return true;
    }
    return input_join_next(in);
}

/* von zeilenanfang bis cursor loeschen, cursor auf 0 (ctrl+u) */
bool input_kill_line(Input *in)
{
    char *line = in->lines[in->cursor_line];
    size_t len = strlen(line);
    if (in->cursor == 0) {
        return false; /* nichts vor dem cursor */
    }
    if (in->cursor > len) {
        in->cursor = len; /* defensively klemmen */
    }
    /* text ab cursor an den anfang schieben (inkl. '\0') */
    memmove(line, line + in->cursor, len - in->cursor + 1);
    in->cursor = 0;
    return true;
}

/* letztes wort vor dem cursor loeschen (inkl. whitespace davor);
 * am zeilenanfang loescht es den umbruch in die vorherige zeile */
bool input_kill_last_word(Input *in)
{
    if (in->cursor == 0) {
        /* am zeilenanfang ist der "umbruch" das letzte wort */
        return input_join_prev(in);
    }
    char *line = in->lines[in->cursor_line];
    size_t len = strlen(line);
    if (in->cursor > len) {
        in->cursor = len; /* defensively klemmen */
    }
    size_t start = in->cursor;
    while (start > 0 && line[start - 1] == ' ') {
        start--; /* whitespace vor dem wort mit loeschen */
    }
    while (start > 0 && line[start - 1] != ' ') {
        start--; /* bis zum wortanfang */
    }
    memmove(line + start, line + in->cursor, len - in->cursor + 1);
    in->cursor = start;
    return true;
}

/* ------------------------------------------------------------------ */
/* Meta-bindings: readline-"woerter" sind alphanumerische sequenzen  */
/* (a-z, A-Z, 0-9), satzzeichen sind grenzen. ASCII reicht hier –   */
/* keine locale-abhaengigkeit gewuenscht.                              */
/* ------------------------------------------------------------------ */

static bool is_alnum(char c)
{
    if (c >= '0' && c <= '9') {
        return true;
    }
    if (c >= 'a' && c <= 'z') {
        return true;
    }
    if (c >= 'A' && c <= 'Z') {
        return true;
    }
    return false;
}

/* an den anfang des vorherigen worts; laeuft ueber zeilengrenzen */
bool input_word_left(Input *in)
{
    for (;;) {
        char *line = in->lines[in->cursor_line];
        size_t pos = in->cursor;
        while (pos > 0 && !is_alnum(line[pos - 1])) {
            pos--; /* sonderzeichen ueberspringen */
        }
        while (pos > 0 && is_alnum(line[pos - 1])) {
            pos--; /* bis zum wortanfang */
        }
        if (pos != in->cursor) {
            in->cursor = pos;
            return true;
        }
        /* in dieser zeile kein wort davor: zeile hoch, am ende
         * weiter suchen */
        if (in->cursor_line == 0) {
            return false; /* anfang von allem */
        }
        in->cursor_line--;
        in->cursor = input_len(in);
    }
}

/* an das ende des naechsten worts; laeuft ueber zeilengrenzen */
bool input_word_right(Input *in)
{
    for (;;) {
        char *line = in->lines[in->cursor_line];
        size_t len = strlen(line);
        size_t pos = in->cursor;
        while (pos < len && !is_alnum(line[pos])) {
            pos++; /* sonderzeichen ueberspringen */
        }
        while (pos < len && is_alnum(line[pos])) {
            pos++; /* bis zum wortende */
        }
        if (pos != in->cursor) {
            in->cursor = pos;
            return true;
        }
        if (in->cursor_line + 1 >= in->count) {
            return false; /* ende von allem */
        }
        in->cursor_line++;
        in->cursor = 0;
    }
}

/* wort ab cursor vorwaerts killen (alt+d) */
bool input_kill_word(Input *in)
{
    char *line = in->lines[in->cursor_line];
    size_t len = strlen(line);
    size_t end = in->cursor;
    while (end < len && !is_alnum(line[end])) {
        end++; /* sonderzeichen ueberspringen */
    }
    while (end < len && is_alnum(line[end])) {
        end++; /* bis zum wortende */
    }
    if (end > in->cursor) {
        memmove(line + in->cursor, line + end, len - end + 1);
        return true;
    }
    /* kein wort mehr in der zeile: umbruch dahinter loeschen */
    return input_join_next(in);
}

/* wort vor dem cursor rueckwaerts killen (alt+backspace) */
bool input_kill_word_back(Input *in)
{
    if (in->cursor == 0) {
        return input_join_prev(in);
    }
    char *line = in->lines[in->cursor_line];
    size_t len = strlen(line);
    if (in->cursor > len) {
        in->cursor = len; /* defensively klemmen */
    }
    size_t start = in->cursor;
    while (start > 0 && !is_alnum(line[start - 1])) {
        start--; /* sonderzeichen ueberspringen */
    }
    while (start > 0 && is_alnum(line[start - 1])) {
        start--; /* bis zum wortanfang */
    }
    if (start == in->cursor) {
        return false; /* nur sonderzeichen, nichts zu killen */
    }
    memmove(line + start, line + in->cursor, len - in->cursor + 1);
    in->cursor = start;
    return true;
}

/* zeichen vor/mit cursor tauschen (ctrl+t) */
bool input_transpose_chars(Input *in)
{
    char *line = in->lines[in->cursor_line];
    size_t len = strlen(line);
    if (in->cursor == 0 || len < 2) {
        return false; /* am anfang braucht readline ein zeichen davor */
    }
    size_t a;
    size_t b;
    if (in->cursor >= len) {
        /* am ende: letzte zwei tauschen, cursor bleibt am ende */
        a = len - 2;
        b = len - 1;
    } else {
        a = in->cursor - 1;
        b = in->cursor;
        in->cursor++;
    }
    char tmp = line[a];
    line[a] = line[b];
    line[b] = tmp;
    return true;
}

/* wort vor dem cursor mit dem wort danach vertauschen (alt+t).
 * readline-tausch um den cursor: das "wort danach" ist das wort,
 * in dem der cursor steht bzw. das direkt dahinter – am zeilen-
 * ende tauscht das die letzten zwei woerter, wie von bash gewohnt */
bool input_transpose_words(Input *in)
{
    char *line = in->lines[in->cursor_line];
    size_t len = strlen(line);

    /* wort B bestimmen */
    size_t bs;
    size_t be;
    if (in->cursor < len && is_alnum(line[in->cursor])) {
        /* cursor steht mitten in einem wort: dieses ist B */
        bs = in->cursor;
        be = bs;
        while (be < len && is_alnum(line[be])) {
            be++;
        }
    } else if (in->cursor > 0 && is_alnum(line[in->cursor - 1])) {
        /* cursor direkt hinter einem wort: dieses ist B */
        be = in->cursor;
        bs = be;
        while (bs > 0 && is_alnum(line[bs - 1])) {
            bs--;
        }
    } else {
        /* auf sonderzeichen: das naechste wort vorwaerts */
        bs = in->cursor;
        while (bs < len && !is_alnum(line[bs])) {
            bs++;
        }
        if (bs >= len) {
            return false; /* kein wort im spiel */
        }
        be = bs;
        while (be < len && is_alnum(line[be])) {
            be++;
        }
    }

    /* wort A: das wort vor B */
    size_t ae = bs;
    while (ae > 0 && !is_alnum(line[ae - 1])) {
        ae--;
    }
    size_t as = ae;
    while (as > 0 && is_alnum(line[as - 1])) {
        as--;
    }
    if (as == ae) {
        return false; /* kein wort davor */
    }

    /* [A][luecke][B] -> [B][luecke][A]: das segment in einem
     * temp-puffer neu zusammensetzen (eine rotation waere nur bei
     * gleich langen woertern korrekt) */
    size_t alen = ae - as;
    size_t gap_len = bs - ae;
    size_t blen = be - bs;
    char *tmp = malloc(alen + gap_len + blen);
    if (!tmp) {
        die("out of memory");
    }
    memcpy(tmp, line + bs, blen);                  /* B vorn */
    memcpy(tmp + blen, line + ae, gap_len);        /* dann die luecke */
    memcpy(tmp + blen + gap_len, line + as, alen); /* dann A */
    memcpy(line + as, tmp, alen + gap_len + blen);
    free(tmp);
    in->cursor = be; /* hinter das (neu hinten liegende) A */
    return true;
}

/* wort ab cursor transformieren, cursor landet an dessen ende.
 * readline-semantik: steht der cursor MITTEN in einem wort, wird
 * nur der rest ab cursor transformiert; auf einer grenze greift
 * das folgende wort komplett. */
typedef enum {
    WORD_UP,   /* ganzes wort gross */
    WORD_DOWN, /* ganzes wort klein */
    WORD_CAP,  /* erster buchstabe gross, rest klein */
} WordCase;

static bool word_case(Input *in, WordCase mode)
{
    char *line = in->lines[in->cursor_line];
    size_t len = strlen(line);

    size_t start = in->cursor;
    if (start < len && !is_alnum(line[start])) {
        /* auf einer grenze: vor zum anfang des naechsten worts */
        while (start < len && !is_alnum(line[start])) {
            start++;
        }
    }
    if (start >= len) {
        return false; /* kein wort mehr */
    }
    size_t end = start;
    while (end < len && is_alnum(line[end])) {
        end++;
    }
    for (size_t k = start; k < end; k++) {
        char c = line[k];
        bool upper = false;
        switch (mode) {
        case WORD_UP:
            upper = true;
            break;
        case WORD_DOWN:
            upper = false;
            break;
        case WORD_CAP:
            upper = (k == start);
            break;
        }
        if (c >= 'a' && c <= 'z' && upper) {
            line[k] = (char)(c - ('a' - 'A'));
        } else if (c >= 'A' && c <= 'Z' && !upper) {
            line[k] = (char)(c + ('a' - 'A'));
        }
    }
    in->cursor = end;
    return true;
}

bool input_word_upcase(Input *in)
{
    return word_case(in, WORD_UP);
}

bool input_word_downcase(Input *in)
{
    return word_case(in, WORD_DOWN);
}

bool input_word_capitalize(Input *in)
{
    return word_case(in, WORD_CAP);
}

void input_char(Input *in, char c)
{
    char *line = in->lines[in->cursor_line];
    size_t len = strlen(line);
    if (len >= INPUT_MAX_LINE_BYTES) {
        return;
    }
    /* defensively klemmen – die invariante sollte immer gelten */
    if (in->cursor > len) {
        in->cursor = len;
    }
    char *grown = realloc(line, len + 2);
    if (!grown) {
        die("out of memory");
    }
    in->lines[in->cursor_line] = grown;
    /* einfuegen AN der cursor-position: der rest (inkl. '\0')
     * rueckt eine position weiter */
    memmove(grown + in->cursor + 1, grown + in->cursor, len - in->cursor + 1);
    grown[in->cursor] = c;
    in->cursor++;
}

int bottom_border_for(int rows, int list_h, bool g_confirm_quit)
{
    int b = rows - 1;
    if (list_h > 0) {
        b -= list_h; /* command-liste unter der input-bar */
    }
    if (g_confirm_quit) {
        b -= 2;
    }
    if (b < 2) {
        b = 2;
    }
    return b;
}

void input_newline(Input *in, int rows, int list_h, bool g_confirm_quit)
{
    int bottom_border = bottom_border_for(rows, list_h, g_confirm_quit);
    if (in->count >= INPUT_MAX_LINES) {
        return;
    }
    if (bottom_border - (int)in->count - 1 < 2) {
        return;
    }

    /* aktuelle zeile an der cursor-position SPLITTEN: der text
     * hinter dem cursor wird die neue zeile. am zeilenende ist das
     * identisch zum alten verhalten (leere zeile anhaengen) */
    char *cur = in->lines[in->cursor_line];
    size_t len = strlen(cur);
    if (in->cursor > len) {
        in->cursor = len; /* defensively klemmen */
    }
    char *tail = dup_str(cur + in->cursor);
    if (!tail) {
        die("out of memory");
    }
    char *head = realloc(cur, in->cursor + 1);
    if (!head) {
        free(tail);
        die("out of memory");
    }
    head[in->cursor] = '\0';
    in->lines[in->cursor_line] = head;

    /* zeilen ab cursor_line+1 eine position nach hinten schieben */
    memmove((void *)&in->lines[in->cursor_line + 2],
            (const void *)&in->lines[in->cursor_line + 1],
            (in->count - in->cursor_line - 1) * sizeof in->lines[0]);
    in->lines[in->cursor_line + 1] = tail;
    in->count++;
    in->cursor_line++; /* cursor am anfang der neuen zeile */
    in->cursor = 0;
}

void input_backspace(Input *in)
{
    char *line = in->lines[in->cursor_line];
    size_t len = strlen(line);
    if (in->cursor > 0 && in->cursor <= len) {
        /* GANZES zeichen vor dem cursor loeschen: umlaut/sz sind
         * zwei bytes, ein halbes wuerde kaputte utf-8-sequenzen im
         * feld hinterlassen */
        size_t n = utf8_before(line, in->cursor);
        if (in->cursor < n) {
            n = in->cursor; /* defensive: randfaelle */
        }
        memmove(line + in->cursor - n, line + in->cursor, len - in->cursor + 1);
        in->cursor -= n;
        return;
    }
    /* am zeilenanfang: den zeilenumbruch davor loeschen. bei einer
     * leeren letzten zeile entfernt das genau die zeile und der
     * cursor landet am ende der zeile davor (wie bisher) */
    (void)input_join_prev(in);
}

void input_reset(Input *in)
{
    input_free(in);
    input_init(in);
}

void input_set_text(Input *in, const char *text)
{
    input_reset(in);
    if (text == NULL) {
        return;
    }
    const char *p = text;
    size_t line = 0;
    for (;;) {
        const char *nl = strchr(p, '\n');
        size_t len = (nl != NULL) ? (size_t)(nl - p) : strlen(p);
        char *copy = malloc(len + 1);
        if (copy == NULL) {
            die("out of memory");
        }
        memcpy(copy, p, len);
        copy[len] = '\0';
        free(in->lines[line]);
        in->lines[line] = copy;
        in->count = line + 1;

        if (nl == NULL || line + 1 >= INPUT_MAX_LINES) {
            break; /* fertig, oder das feld ist voll */
        }
        line++;
        p = nl + 1;
    }
    in->cursor_line = in->count - 1;
    in->cursor = strlen(in->lines[in->cursor_line]);
}

void input_init(Input *in)
{
    memset(in, 0, sizeof *in);
    in->lines[0] = calloc(1, 1);
    if (!in->lines[0]) {
        die("out of memory");
    }
    in->count = 1;
}

/* ------------------------------------------------------------------ */
/* Soft-wrap (siehe input.h): eine logische zeile wird zeichenweise   */
/* an der feldkante umgebrochen. alle funktionen hier lesen nur –     */
/* der text aendert sich durch den umbruch nie.                       */
/* ------------------------------------------------------------------ */

/* ein codepoint ab s: byte-laenge. defekte sequenzen zaehlen als
 * einzelbyte, gelesen wird nie ueber den terminator hinaus. */
/* byte-laenge des utf-8-zeichens, das BEI byte i beginnt (cursor
 * steht auf dem zeichenanfang). kaputte/alleinige bytes: 1 */
static size_t utf8_at(const char *s, size_t i)
{
    unsigned char c = (unsigned char)s[i];
    if ((c & 0xE0U) == 0xC0U) {
        return 2;
    }
    if ((c & 0xF0U) == 0xE0U) {
        return 3;
    }
    if ((c & 0xF8U) == 0xF0U) {
        return 4;
    }
    return 1;
}

/* byte-laenge des utf-8-zeichens, das VOR byte i ENDET. der cursor
 * muss auf einer zeichengrenze stehen; von dort rueckwaerts zum
 * lead-byte laufen (maximal 3 continuation-bytes) */
static size_t utf8_before(const char *s, size_t i)
{
    size_t n = 0;
    while (n < 3 && i > n && (((unsigned char)s[i - n - 1] & 0xC0U) == 0x80U)) {
        n++;
    }
    return n + 1;
}

static size_t wrap_step(const char *s)
{
    unsigned char c = (unsigned char)s[0];
    size_t n = 1;
    if ((c & 0xE0U) == 0xC0U) {
        n = 2;
    } else if ((c & 0xF0U) == 0xE0U) {
        n = 3;
    } else if ((c & 0xF8U) == 0xF0U) {
        n = 4;
    }
    for (size_t i = 1; i < n; i++) {
        if (((unsigned char)s[i] & 0xC0U) != 0x80U) {
            return 1; /* abgebrochene sequenz */
        }
    }
    return n;
}

/* breite auf etwas sinnvolles klemmen: 0 oder negativ waere eine
 * endlosschleife, und ein feld unter einer zelle gibt es nicht */
static size_t wrap_width(int width)
{
    return (width > 0) ? (size_t)width : 1;
}

/* wieviele bildschirmzeilen belegt EINE logische zeile? */
static size_t line_rows(const char *s, size_t w)
{
    size_t rows = 1;
    size_t cells = 0;
    for (size_t i = 0; s[i] != '\0';) {
        if (cells == w) {
            rows++;
            cells = 0;
        }
        i += wrap_step(s + i);
        cells++;
    }
    return rows;
}

/* den n-ten abschnitt EINER logischen zeile: byte-offset + laenge.
 * n muss < line_rows(s, w) sein. */
static void line_slice(const char *s, size_t w, size_t n, size_t *off,
                       size_t *len)
{
    size_t row = 0;
    size_t start = 0;
    size_t cells = 0;
    size_t i = 0;
    while (s[i] != '\0') {
        if (cells == w) {
            if (row == n) {
                *off = start;
                *len = i - start;
                return;
            }
            row++;
            start = i;
            cells = 0;
        }
        i += wrap_step(s + i);
        cells++;
    }
    *off = start;
    *len = i - start; /* letzter abschnitt bis zum zeilenende */
}

size_t input_screen_rows(const Input *in, int width)
{
    if (in == NULL) {
        return 0;
    }
    size_t w = wrap_width(width);
    size_t rows = 0;
    for (size_t i = 0; i < in->count; i++) {
        rows += line_rows(in->lines[i], w);
    }
    return rows;
}

bool input_screen_row(const Input *in, int width, size_t idx, size_t *line,
                      size_t *off, size_t *len)
{
    if (in == NULL) {
        return false;
    }
    size_t w = wrap_width(width);
    size_t seen = 0;
    for (size_t i = 0; i < in->count; i++) {
        size_t rows = line_rows(in->lines[i], w);
        if (idx < seen + rows) {
            size_t o = 0;
            size_t l = 0;
            line_slice(in->lines[i], w, idx - seen, &o, &l);
            if (line != NULL) {
                *line = i;
            }
            if (off != NULL) {
                *off = o;
            }
            if (len != NULL) {
                *len = l;
            }
            return true;
        }
        seen += rows;
    }
    return false;
}

void input_cursor_screen(const Input *in, int width, size_t *row, size_t *col)
{
    size_t r = 0;
    size_t c = 0;
    if (in == NULL) {
        goto out;
    }
    size_t w = wrap_width(width);
    for (size_t i = 0; i < in->cursor_line && i < in->count; i++) {
        r += line_rows(in->lines[i], w);
    }
    /* in der cursor-zeile bis zur cursor-position mitzaehlen */
    const char *s = in->lines[in->cursor_line];
    size_t cur = in->cursor;
    if (cur > strlen(s)) {
        cur = strlen(s);
    }
    for (size_t i = 0; i < cur;) {
        if (c == w) {
            r++;
            c = 0;
        }
        i += wrap_step(s + i);
        c++;
    }
out:
    if (row != NULL) {
        *row = r;
    }
    if (col != NULL) {
        *col = c;
    }
}

/* cursor auf (bildschirmzeile, spalte) setzen. die spalte wird auf
 * die laenge des abschnitts geklemmt. */
static void cursor_to_screen(Input *in, int width, size_t row, size_t col)
{
    size_t line = 0;
    size_t off = 0;
    size_t len = 0;
    if (!input_screen_row(in, width, row, &line, &off, &len)) {
        return;
    }
    /* col zellen in den abschnitt hinein, utf-8-weise */
    const char *s = in->lines[line] + off;
    size_t i = 0;
    size_t cells = 0;
    while (cells < col && i < len) {
        i += wrap_step(s + i);
        cells++;
    }
    in->cursor_line = line;
    in->cursor = off + i;
}

bool input_screen_up(Input *in, int width)
{
    size_t row = 0;
    size_t col = 0;
    input_cursor_screen(in, width, &row, &col);
    if (row == 0) {
        return false; /* schon in der obersten zeile */
    }
    cursor_to_screen(in, width, row - 1, col);
    return true;
}

bool input_screen_down(Input *in, int width)
{
    size_t row = 0;
    size_t col = 0;
    input_cursor_screen(in, width, &row, &col);
    if (row + 1 >= input_screen_rows(in, width)) {
        return false; /* schon in der untersten zeile */
    }
    cursor_to_screen(in, width, row + 1, col);
    return true;
}
