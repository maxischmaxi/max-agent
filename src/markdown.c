/* markdown.c: block-scan + zeilen-highlight fuer ki-antworten.
 * siehe markdown.h fuer die design-gruende (streaming-sicherheit,
 * block-zustand, tabellen-layout). */

#include "markdown.h"

#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* hilfsfunktionen: zeilen-grenzen                                     */
/* ------------------------------------------------------------------ */

/* ist [off,len) nur leerzeichen? */
static bool is_blank(const char *s, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        if (s[i] != ' ' && s[i] != '\t') {
            return false;
        }
    }
    return true;
}

/* zaehlt '|' in [off,len), ausser in escapes/quotes – rudimentaer:
 * nur zaehlen, markdown-tabellen haben keine gequoteten pipes */
static size_t count_pipes(const char *s, size_t len)
{
    size_t n = 0;
    for (size_t i = 0; i < len; i++) {
        if (s[i] == '|') {
            n++;
        }
    }
    return n;
}

/* ------------------------------------------------------------------ */
/* zeilenlokaler scanner (headline/liste/zitat) – unveraendert       */
/* ------------------------------------------------------------------ */

static size_t scan_num_marker(const char *s, size_t len)
{
    size_t i = 0;
    while (i < len && s[i] >= '0' && s[i] <= '9') {
        i++;
    }
    if (i == 0 || i >= len) {
        return 0;
    }
    if (s[i] != '.' && s[i] != ')') {
        return 0;
    }
    return i + 1;
}

void md_scan(const char *text, size_t off, size_t len, bool line_start,
             MdLine *out)
{
    out->kind = MD_NONE;
    out->level = 0;
    out->marker = off;
    out->marker_len = 0;

    if (!line_start || len == 0) {
        return;
    }

    const char *s = text + off;

    size_t i = 0;
    while (i < len && i < 3 && s[i] == ' ') {
        i++;
    }
    if (i >= len) {
        return;
    }

    if (s[i] == '#') {
        size_t h = i;
        size_t n = 0;
        while (h < len && s[h] == '#') {
            h++;
            n++;
        }
        if (h < len && s[h] == ' ') {
            out->kind = MD_HEAD;
            out->level = (int)n;
            out->marker = off + i;
            out->marker_len = h - i;
            return;
        }
        return;
    }

    if ((s[i] == '-' || s[i] == '*' || s[i] == '+') && i + 1 < len &&
        s[i + 1] == ' ') {
        out->kind = MD_LIST;
        out->marker = off + i;
        out->marker_len = 1;
        return;
    }

    size_t num = scan_num_marker(s + i, len - i);
    if (num > 0) {
        out->kind = MD_LIST;
        out->marker = off + i;
        out->marker_len = num;
        return;
    }

    if (s[i] == '>') {
        out->kind = MD_QUOTE;
        out->marker = off + i;
        out->marker_len = 1;
        return;
    }
}

/* ------------------------------------------------------------------ */
/* block-scan: fences + tabellen                                      */
/* ------------------------------------------------------------------ */

/* fence-zeile: 3+ backticks, optional sprache dahinter. rueckgabe:
 * laenge der fence-sequenz ab s, 0 = keine fence. */
static size_t fence_len(const char *s, size_t len)
{
    if (len < 3 || s[0] != '`' || s[1] != '`' || s[2] != '`') {
        return 0;
    }
    size_t n = 0;
    while (n < len && s[n] == '`') {
        n++;
    }
    return n;
}

/* tabellen-trenner-zeile: nur '|', '-', ':', leerzeichen – und
 * mindestens ein '-' damit's nicht die kopfzeile selbst ist */
static bool is_table_sep(const char *s, size_t len)
{
    bool has_dash = false;
    for (size_t i = 0; i < len; i++) {
        char c = s[i];
        if (c == '-') {
            has_dash = true;
        } else if (c != '|' && c != ':' && c != ' ' && c != '\t') {
            return false;
        }
    }
    return has_dash;
}

/* hat die zeile '|' und spalten-inhalt (keine leere zeile)? */
static bool looks_like_table(const char *s, size_t len)
{
    return count_pipes(s, len) >= 2 && !is_blank(s, len);
}

void md_block_scan(const char *text, MdBlocks *out)
{
    out->n = 0;
    if (text == NULL) {
        return;
    }
    size_t tlen = strlen(text);

    size_t i = 0;
    while (i < tlen) {
        size_t ls = i;
        size_t le = i;
        while (le < tlen && text[le] != '\n') {
            le++;
        }
        size_t llen = le - ls;
        const char *line = text + ls;

        /* --- code-fence --- */
        size_t fl = fence_len(line, llen);
        if (fl > 0 && out->n < MD_BLOCKS_MAX) {
            /* sprache: rest der zeile nach den ticks, getrimmt */
            const char *lang = "";
            if (fl < llen) {
                size_t ls2 = ls + fl;
                while (ls2 < le && (text[ls2] == ' ' || text[ls2] == '\t')) {
                    ls2++;
                }
                size_t le2 = le;
                while (le2 > ls2 &&
                       (text[le2 - 1] == ' ' || text[le2 - 1] == '\t')) {
                    le2--;
                }
                if (le2 > ls2) {
                    /* lang zeigt in den text: mit '\0' endet die
                     * zeile am '\n' – sprach-string nicht kopierbar.
                     * deshalb: kopie vermeiden, stattdessen zeigt
                     * lang auf den zeilenanfang und wir merken uns
                     * die laenge nicht (tokenizer vergleicht nur
                     * prefix bis zeilenende). */
                    lang = text + ls2;
                }
            }
            /* schliessende fence suchen: gleiche anzahl ticks am
             * zeilenanfang, weitere zeilen. text-ende schliesst
             * den block (streaming: der block ist solange offen) */
            size_t body = le + 1;
            if (body > tlen) {
                body = tlen;
            }
            size_t j = body;
            size_t end = tlen;
            while (j < tlen) {
                size_t js = j;
                size_t je = j;
                while (je < tlen && text[je] != '\n') {
                    je++;
                }
                /* nur die fence-sequenz zaehlt (sprache ist beim
                 * schliesser unueblich, aber erlaubt) */
                if (fence_len(text + js, je - js) >= fl) {
                    /* incl. schliessender zeile; an der text-
                     * grenze klemmen (je == tlen ohne '\n':
                     * je+1 laege ausserhalb, chat_wrap laeuft
                     * sonst uebers terminator-byte) */
                    end = (je < tlen) ? je + 1 : tlen;
                    break;
                }
                j = je + 1;
            }
            out->blocks[out->n].kind = MD_BLK_CODE;
            out->blocks[out->n].start = ls;
            out->blocks[out->n].end = end;
            out->blocks[out->n].lang = lang;
            out->n++;
            i = end;
            continue;
        }

        /* --- tabelle: kopf + trenner + datenzeilen --- */
        if (looks_like_table(line, llen) && out->n < MD_BLOCKS_MAX) {
            size_t j = le + 1;
            if (j < tlen) {
                size_t js = j;
                size_t je = j;
                while (je < tlen && text[je] != '\n') {
                    je++;
                }
                if (is_table_sep(text + js, je - js)) {
                    /* datenzeilen: solange tabellen-zeilen kommen.
                     * je+1 an der text-grenze klemmen (s.o.) */
                    size_t end = (je < tlen) ? je + 1 : tlen;
                    size_t k = end;
                    while (k < tlen) {
                        size_t ks = k;
                        size_t ke = k;
                        while (ke < tlen && text[ke] != '\n') {
                            ke++;
                        }
                        if (!looks_like_table(text + ks, ke - ks)) {
                            break;
                        }
                        end = (ke < tlen) ? ke + 1 : tlen;
                        k = end;
                    }
                    out->blocks[out->n].kind = MD_BLK_TABLE;
                    out->blocks[out->n].start = ls;
                    out->blocks[out->n].end = end;
                    out->blocks[out->n].lang = "";
                    out->n++;
                    i = end;
                    continue;
                }
            }
        }

        i = le + 1;
    }
}

const MdBlock *md_block_at(const MdBlocks *blks, size_t off)
{
    if (blks == NULL) {
        return NULL;
    }
    for (int b = 0; b < blks->n; b++) {
        if (off >= blks->blocks[b].start && off < blks->blocks[b].end) {
            return &blks->blocks[b];
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* code-tokenisierung                                                   */
/* ------------------------------------------------------------------ */

/* sprach-keywords: je tabelle eine sprache. vergleich GROSS-/klein-
 * schreibung je nach sprache (c/bash klein, bash-kommandos egal).
 * die listen sind klein und decken die haeufigsten sprachen ab. */
typedef struct {
    const char *lang;
    bool ci; /* case-insensitive? */
    const char *const *kw;
    size_t kw_n;
    const char *line_comment; /* NULL = keine */
    const char *block_comment_open;
    const char *block_comment_close;
} MdLang;

static const char *const KW_C[] = {
    "int", "char", "long", "short", "unsigned", "signed", "float",
    "double", "void", "const", "static", "extern", "inline", "struct",
    "enum", "union", "typedef", "sizeof", "return", "if", "else",
    "for", "while", "do", "switch", "case", "default", "break",
    "continue", "goto", "true", "false", "NULL", "include", "define",
    NULL,
};
static const char *const KW_BASH[] = {
    "if", "then", "else", "elif", "fi", "for", "while", "until", "do",
    "done", "case", "esac", "in", "function", "return", "local",
    "export", "readonly", "echo", "printf", "read", "cd", "ls", "cat",
    "grep", "sed", "awk", "find", "xargs", "chmod", "chown", "mkdir",
    "rm", "cp", "mv", "ln", "touch", "true", "false", "test", "set",
    "source", "exit", "sudo", "make", "git", "curl", "tar", "which",
    NULL,
};
static const char *const KW_PY[] = {
    "def", "return", "if", "elif", "else", "for", "while", "in", "not",
    "and", "or", "is", "None", "True", "False", "class", "import",
    "from", "as", "try", "except", "finally", "raise", "with", "lambda",
    "pass", "break", "continue", "global", "nonlocal", "yield", "del",
    "assert", "async", "await", "self", "print", "len", "range", "str",
    "int", "float", "list", "dict", "set", "tuple",
    NULL,
};
static const char *const KW_JS[] = {
    "function", "return", "if", "else", "for", "while", "do", "switch",
    "case", "default", "break", "continue", "const", "let", "var",
    "class", "extends", "new", "this", "super", "import", "export",
    "from", "async", "await", "try", "catch", "finally", "throw",
    "typeof", "instanceof", "in", "of", "delete", "void", "yield",
    "true", "false", "null", "undefined", "console",
    NULL,
};
static const char *const KW_GO[] = {
    "func", "package", "import", "return", "if", "else", "for", "range",
    "switch", "case", "default", "break", "continue", "type", "struct",
    "interface", "map", "chan", "go", "defer", "select", "var", "const",
    "nil", "true", "false", "string", "int", "int64", "uint", "float64",
    "byte", "rune", "error", "make", "new", "append", "len", "cap",
    "panic", "recover",
    NULL,
};
static const char *const KW_RS[] = {
    "fn", "let", "mut", "const", "static", "if", "else", "match", "for",
    "while", "loop", "break", "continue", "return", "struct", "enum",
    "trait", "impl", "pub", "use", "mod", "crate", "self", "super",
    "where", "as", "in", "ref", "move", "async", "await", "dyn", "box",
    "true", "false", "Some", "None", "Ok", "Err", "unsafe",
    NULL,
};
static const char *const KW_SQL[] = {
    "SELECT", "FROM", "WHERE", "INSERT", "INTO", "VALUES", "UPDATE",
    "SET", "DELETE", "CREATE", "TABLE", "DROP", "ALTER", "ADD", "JOIN",
    "LEFT", "RIGHT", "INNER", "OUTER", "ON", "AS", "AND", "OR", "NOT",
    "NULL", "IS", "IN", "LIKE", "BETWEEN", "ORDER", "BY", "GROUP",
    "HAVING", "LIMIT", "OFFSET", "DISTINCT", "COUNT", "SUM", "AVG",
    "MIN", "MAX", "PRIMARY", "KEY", "FOREIGN", "REFERENCES", "INDEX",
    NULL,
};

/* sprach-alias: prefix-match reicht ("c++" haengt an "c" fest),
 * lang zeigt auf den fence-rest OHNE terminator – wir vergleichen
 * nur so weit, wie die zeile lang ist */
static const MdLang *lang_of(const char *lang)
{
    if (lang == NULL || lang[0] == '\0') {
        return NULL;
    }
    static const MdLang LANGS[] = {
        {"c", false, KW_C, sizeof KW_C / sizeof KW_C[0], "//", "/*", "*/"},
        {"h", false, KW_C, sizeof KW_C / sizeof KW_C[0], "//", "/*", "*/"},
        {"cpp", false, KW_C, sizeof KW_C / sizeof KW_C[0], "//", "/*", "*/"},
        {"java", false, KW_C, sizeof KW_C / sizeof KW_C[0], "//", "/*", "*/"},
        {"sh", true, KW_BASH, sizeof KW_BASH / sizeof KW_BASH[0], "#", NULL,
         NULL},
        {"bash", true, KW_BASH, sizeof KW_BASH / sizeof KW_BASH[0], "#", NULL,
         NULL},
        {"zsh", true, KW_BASH, sizeof KW_BASH / sizeof KW_BASH[0], "#", NULL,
         NULL},
        {"console", true, KW_BASH, sizeof KW_BASH / sizeof KW_BASH[0], "#",
         NULL, NULL},
        {"py", false, KW_PY, sizeof KW_PY / sizeof KW_PY[0], "#", NULL, NULL},
        {"python", false, KW_PY, sizeof KW_PY / sizeof KW_PY[0], "#", NULL,
         NULL},
        {"js", false, KW_JS, sizeof KW_JS / sizeof KW_JS[0], "//", "/*", "*/"},
        {"ts", false, KW_JS, sizeof KW_JS / sizeof KW_JS[0], "//", "/*", "*/"},
        {"javascript", false, KW_JS, sizeof KW_JS / sizeof KW_JS[0], "//",
         "/*", "*/"},
        {"typescript", false, KW_JS, sizeof KW_JS / sizeof KW_JS[0], "//",
         "/*", "*/"},
        {"go", false, KW_GO, sizeof KW_GO / sizeof KW_GO[0], "//", "/*", "*/"},
        {"rs", false, KW_RS, sizeof KW_RS / sizeof KW_RS[0], "//", "/*", "*/"},
        {"rust", false, KW_RS, sizeof KW_RS / sizeof KW_RS[0], "//", "/*",
         "*/"},
        {"sql", true, KW_SQL, sizeof KW_SQL / sizeof KW_SQL[0], "--", NULL,
         NULL},
    };
    size_t n = sizeof LANGS / sizeof LANGS[0];
    for (size_t i = 0; i < n; i++) {
        /* lang hat keinen terminator (zeigt in den nachricht-text):
         * vergleichen bis zur laenge des namens – der rest der
         * fence-zeile gehoert nicht mehr zur sprachbezeichnung
         * (prefix-match: "c++" -> "c") */
        size_t nl = strlen(LANGS[i].lang);
        if (strncmp(lang, LANGS[i].lang, nl) == 0) {
            return &LANGS[i];
        }
    }
    return NULL;
}

/* wortanfang/-ende: identifier-zeichen */
static bool is_word_byte(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_';
}

static size_t kw_match(const MdLang *L, const char *s, size_t len)
{
    if (L == NULL) {
        return 0;
    }
    size_t wlen = 0;
    while (wlen < len && is_word_byte(s[wlen])) {
        wlen++;
    }
    for (size_t k = 0; k < L->kw_n; k++) {
        const char *kw = L->kw[k];
        if (kw == NULL) {
            continue;
        }
        size_t kl = strlen(kw);
        if (kl != wlen) {
            continue;
        }
        bool eq;
        if (L->ci) {
            eq = true;
            for (size_t a = 0; a < kl; a++) {
                char x = s[a];
                char y = kw[a];
                if (x >= 'A' && x <= 'Z') {
                    x = (char)(x + ('a' - 'A'));
                }
                if (y >= 'A' && y <= 'Z') {
                    y = (char)(y + ('a' - 'A'));
                }
                if (x != y) {
                    eq = false;
                    break;
                }
            }
        } else {
            eq = (strncmp(s, kw, kl) == 0);
        }
        if (eq) {
            return wlen;
        }
    }
    return 0;
}

size_t md_code_token(const char *text, size_t i, const char *lang, MdTok *tok)
{
    size_t tlen = strlen(text);
    if (i >= tlen) {
        return 0;
    }
    tok->kind = MD_TOK_PLAIN;
    tok->off = i;
    tok->len = 1;

    const MdLang *L = lang_of(lang);

    char c = text[i];

    /* string-literal: '...' oder "..." bis zum schliesser, zeilen-
     * ende oder text-ende (streaming: offen bleibt offen, der
     * naechste frame re-scant) */
    if (c == '"' || c == '\'') {
        size_t j = i + 1;
        while (j < tlen && text[j] != c && text[j] != '\n') {
            if (text[j] == '\\' && j + 1 < tlen && text[j + 1] != '\n') {
                j += 2; /* escape ueberspringen */
            } else {
                j++;
            }
        }
        if (j < tlen && text[j] == c) {
            j++; /* schliesser dazu */
        }
        tok->kind = MD_TOK_STR;
        tok->len = j - i;
        return tok->len;
    }

    /* block-kommentar (nur mit schliesser-zeile in der sprache) */
    if (L != NULL && L->block_comment_open != NULL &&
        strncmp(text + i, L->block_comment_open,
                strlen(L->block_comment_open)) == 0) {
        const char *cl = L->block_comment_close;
        size_t j = i + strlen(L->block_comment_open);
        while (j < tlen) {
            if (cl != NULL && strncmp(text + j, cl, strlen(cl)) == 0) {
                j += strlen(cl);
                break;
            }
            j++;
        }
        tok->kind = MD_TOK_COMMENT;
        tok->len = j - i;
        return tok->len;
    }

    /* zeilen-kommentar bis '\n' (offen = bis text-ende) */
    if (L != NULL && L->line_comment != NULL &&
        strncmp(text + i, L->line_comment, strlen(L->line_comment)) == 0) {
        size_t j = i;
        while (j < tlen && text[j] != '\n') {
            j++;
        }
        tok->kind = MD_TOK_COMMENT;
        tok->len = j - i;
        return tok->len;
    }

    /* zahl: ziffern (hex 0x auch) */
    if ((c >= '0' && c <= '9') ||
        (c == '-' && i + 1 < tlen && text[i + 1] >= '0' &&
         text[i + 1] <= '9')) {
        size_t j = i + 1;
        if (text[i] == '0' && j < tlen && (text[j] == 'x' || text[j] == 'X')) {
            j++;
            while (j < tlen && is_word_byte(text[j])) {
                j++;
            }
        } else {
            while (j < tlen && (is_word_byte(text[j]) || text[j] == '.')) {
                j++;
            }
        }
        tok->kind = MD_TOK_NUM;
        tok->len = j - i;
        return tok->len;
    }

    /* schluesselwort / identifier */
    if (is_word_byte(c)) {
        size_t wlen = kw_match(L, text + i, tlen - i);
        if (wlen > 0) {
            tok->kind = MD_TOK_KW;
            tok->len = wlen;
            return wlen;
        }
        /* identifier: ganz normal, aber als EIN token (damit der
         * tokenizer durchs ganze wort laeuft) */
        size_t j = i;
        while (j < tlen && is_word_byte(text[j])) {
            j++;
        }
        tok->kind = MD_TOK_PLAIN;
        tok->len = j - i;
        return j - i;
    }

    /* sonderzeichen: einzeln */
    return 1;
}

/* ------------------------------------------------------------------ */
/* tabellen-layout                                                     */
/* ------------------------------------------------------------------ */

/* eine zeile in spalten zerlegen: '|' als trenner, rand-pipes weg,
 * zellen getrimmt. gibt die anzahl und die byte-offsets/laengen in
 * die tabelle col_off/col_len (max MD_TABLE_COLS_MAX spalten). */
#define MD_TABLE_COLS_MAX 32

static size_t table_row(const char *text, size_t ls, size_t le,
                        size_t *col_off, size_t *col_len, size_t max_cols)
{
    /* randpipes ueberspringen */
    size_t i = ls;
    if (i < le && text[i] == '|') {
        i++;
    }
    size_t n = 0;
    while (i <= le && n < max_cols) {
        size_t cs = i;
        while (cs < le && (text[cs] == ' ' || text[cs] == '\t')) {
            cs++;
        }
        size_t ce = cs;
        while (ce < le && text[ce] != '|') {
            ce++;
        }
        size_t ce2 = ce;
        while (ce2 > cs && (text[ce2 - 1] == ' ' || text[ce2 - 1] == '\t')) {
            ce2--;
        }
        /* leere zelle am rand (trailing pipe): nicht mitnehmen */
        if (cs >= ce2 && ce >= le) {
            break;
        }
        col_off[n] = cs;
        col_len[n] = ce2 - cs;
        n++;
        if (ce >= le) {
            break;
        }
        i = ce + 1;
    }
    return n;
}

/* sichtbare zellen einer tabellen-zeile (fuer die breite) */
static size_t cells_len(const char *text, size_t off, size_t len)
{
    /* utf-8: ein codepoint = 1 zelle (wie ueberall im renderer) */
    size_t cells = 0;
    size_t i = 0;
    while (i < len) {
        unsigned char c = (unsigned char)text[off + i];
        size_t n = 1;
        if ((c & 0xE0U) == 0xC0U) {
            n = 2;
        } else if ((c & 0xF0U) == 0xE0U) {
            n = 3;
        } else if ((c & 0xF8U) == 0xF0U) {
            n = 4;
        }
        for (size_t a = 1; a < n; a++) {
            if (((unsigned char)text[off + i + a] & 0xC0U) != 0x80U) {
                n = 1;
                break;
            }
        }
        cells++;
        i += n;
    }
    return cells;
}

char *md_table_display(const char *text, size_t start, size_t end,
                       int full_width)
{
    if (text == NULL || start >= end) {
        return NULL;
    }

    /* zeilen einsammeln: kopf, trenner (uebersprungen), daten */
    size_t row_off[64];
    size_t row_end[64];
    size_t rows = 0;
    size_t i = start;
    bool sep_next = false; /* nach der kopfzeile kommt der trenner */
    while (i < end && rows < 64) {
        size_t ls = i;
        size_t le = i;
        while (le < end && text[le] != '\n') {
            le++;
        }
        if (!sep_next) {
            row_off[rows] = ls;
            row_end[rows] = le;
            rows++;
            sep_next = (rows == 1); /* die zeile nach dem kopf ist
                                    * der trenner (|---|---|) */
        } else {
            sep_next = false; /* genau diese eine zeile ueberspringen */
        }
        i = le + 1;
    }
    if (rows == 0) {
        return NULL;
    }

    /* spalten je zeile */
    static size_t coff[64][MD_TABLE_COLS_MAX];
    static size_t clen[64][MD_TABLE_COLS_MAX];
    size_t cols = 0;
    for (size_t r = 0; r < rows; r++) {
        size_t n = table_row(text, row_off[r], row_end[r], coff[r], clen[r],
                             MD_TABLE_COLS_MAX);
        if (n > cols) {
            cols = n;
        }
    }
    if (cols == 0) {
        return NULL;
    }

    /* spaltenbreiten: laengster inhalt, in zellen */
    size_t colw[MD_TABLE_COLS_MAX] = {0};
    for (size_t r = 0; r < rows; r++) {
        for (size_t c = 0; c < MD_TABLE_COLS_MAX; c++) {
            size_t w = (c < clen[r][0] || c < MD_TABLE_COLS_MAX)
                           ? ((coff[r][c] < row_end[r])
                                  ? cells_len(text, coff[r][c], clen[r][c])
                                  : 0)
                           : 0;
            if (w > colw[c]) {
                colw[c] = w;
            }
        }
    }

    /* gesamt + rest verteilen: tabelle ueber die volle breite. die
     * restliche breite (full_width - minimum) geht gleichmaessig
     * an die spalten: breite spalten werden breiter als schmale,
     * alle wachsen proportional. */
    size_t sep = 3; /* " | " */
    size_t min = 0;
    for (size_t c = 0; c < cols; c++) {
        min += colw[c];
    }
    min += sep * (cols - 1);
    int avail = full_width - 2; /* padding links/rechts abziehen */
    if (avail < (int)min) {
        avail = (int)min; /* passt nicht: minimum halten */
    }
    size_t extra = (size_t)(avail - (int)min);
    size_t each = (cols > 0) ? extra / cols : 0;
    size_t rem = (cols > 0) ? extra % cols : 0;
    for (size_t c = 0; c < cols; c++) {
        colw[c] += each + ((c < rem) ? 1 : 0);
    }

    /* ausgabe-string bauen: je zeile " zelle | zelle ... "
     * (padding links/rechts wie nachrichten) */
    size_t cap = ((size_t)full_width + 8) * rows + 64;
    char *out = malloc(cap);
    if (out == NULL) {
        return NULL;
    }
    size_t pos = 0;
    for (size_t r = 0; r < rows; r++) {
        out[pos++] = ' '; /* linkes padding */
        for (size_t c = 0; c < cols; c++) {
            if (c > 0) {
                out[pos++] = ' ';
                out[pos++] = '|';
                out[pos++] = ' ';
            }
            size_t cell_off = coff[r][c];
            size_t cell_len = clen[r][c];
            if (cell_off == 0 && cell_len == 0) {
                cell_len = 0;
            }
            /* zellinhalt + pad bis spaltenbreite */
            size_t cells = 0;
            if (cell_off < row_end[r] || cell_len > 0) {
                for (size_t b = 0; b < cell_len; b++) {
                    out[pos++] = text[cell_off + b];
                }
                cells = cells_len(text, cell_off, cell_len);
            }
            while (cells < colw[c]) {
                out[pos++] = ' ';
                cells++;
            }
        }
        out[pos++] = ' '; /* rechtes padding */
        out[pos++] = '\n';
    }
    out[pos] = '\0';
    return out;
}