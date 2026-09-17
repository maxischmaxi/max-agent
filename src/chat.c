#include "chat.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "markdown.h"
#include "utils.h"

/* ------------------------------------------------------------------ */
/* wachstum: kapazitaet verdoppeln, startwert 8. wie ueblich bleibt  */
/* chat->msgs NULL, solange nichts angehaengt wurde (zero-init ok).  */
/* ------------------------------------------------------------------ */
bool chat_role_sent(ChatRole role)
{
    switch (role) {
    case CHAT_ROLE_SYSTEM:
    case CHAT_ROLE_USER:
    case CHAT_ROLE_ASSISTANT:
    case CHAT_ROLE_TOOL:
        return true;
    case CHAT_ROLE_ERROR:
    case CHAT_ROLE_NOTICE:
        return false;
    }
    return false;
}

static int chat_reserve(Chat *chat, size_t need)
{
    if (chat->cap >= need) {
        return 0;
    }
    size_t cap = (chat->cap > 0) ? chat->cap : 8;
    while (cap < need) {
        if (cap > SIZE_MAX / 2) {
            return -1;
        }
        cap *= 2;
    }
    /* sizeof *msgs kann 0 theoretisch nicht sein, aber die pruefung
     * schuetzt gegen kapazitaets-overflow beim realloc */
    if (cap > SIZE_MAX / sizeof *chat->msgs) {
        return -1;
    }
    ChatMessage *msgs = realloc(chat->msgs, cap * sizeof *msgs);
    if (msgs == NULL) {
        return -1;
    }
    chat->msgs = msgs;
    chat->cap = cap;
    return 0;
}

int chat_append(Chat *chat, ChatRole role, const char *text)
{
    if (text == NULL || chat_reserve(chat, chat->len + 1) != 0) {
        return -1;
    }
    /* CR hat im frame-buffer nichts verloren (es wuerde den zeilen-
     * cursor des terminals in spalte 0 zurueckwerfen): beim anhaengen
     * rausfiltern. n ist nur die obere grenze, der rest passt immer. */
    size_t n = strlen(text) + 1;
    char *copy = malloc(n);
    if (copy == NULL) {
        return -1;
    }
    char *p = copy;
    for (const char *s = text; *s != '\0'; s++) {
        if (*s != '\r') {
            *p++ = *s;
        }
    }
    *p = '\0';
    ChatMessage *m = &chat->msgs[chat->len];
    m->role = role;
    m->text = copy;
    m->tool_calls = NULL;
    m->tool_calls_len = 0;
    m->tool_call_id = NULL;
    chat->len++;
    return 0;
}

int chat_append_tool(Chat *chat, const char *tool_call_id, const char *result)
{
    if (tool_call_id == NULL || result == NULL) {
        return -1;
    }
    if (chat_append(chat, CHAT_ROLE_TOOL, result) != 0) {
        return -1;
    }
    ChatMessage *m = &chat->msgs[chat->len - 1];
    m->tool_call_id = dup_str(tool_call_id);
    if (m->tool_call_id == NULL) {
        chat_pop(chat);
        return -1;
    }
    return 0;
}

/* alle heap-felder einer nachricht freigeben und auf NULL setzen */
static void msg_free_fields(ChatMessage *m)
{
    for (size_t i = 0; i < m->tool_calls_len; i++) {
        free(m->tool_calls[i].id);
        free(m->tool_calls[i].name);
        free(m->tool_calls[i].arguments);
    }
    free(m->tool_calls);
    m->tool_calls = NULL;
    m->tool_calls_len = 0;
    free(m->tool_call_id);
    m->tool_call_id = NULL;
    free(m->text);
    m->text = NULL;
}

bool chat_pop(Chat *chat)
{
    if (chat->len == 0) {
        return false;
    }
    chat->len--;
    msg_free_fields(&chat->msgs[chat->len]);
    return true;
}

bool chat_append_text(Chat *chat, const char *text)
{
    if (chat == NULL || chat->len == 0) {
        return false; /* nichts zum anhaengen */
    }
    if (text == NULL) {
        return false;
    }
    ChatMessage *m = &chat->msgs[chat->len - 1];
    if (m->text == NULL) {
        return false; /* darf laut invariant nicht passieren */
    }
    size_t old_len = strlen(m->text);
    size_t add = strlen(text);
    if (add == 0) {
        return true; /* no-op, aber kein fehler */
    }
    if (add > SIZE_MAX - old_len - 1) {
        return false; /* laenge ueberlaeuft size_t */
    }
    /* realloc: die nachricht bleibt die letzte, das msgs-array
     * (und damit alle anderen text-pointer) bewegt sich nicht –
     * die vom sende-modul geborgten pointer bleiben gueltig */
    char *grown = realloc(m->text, old_len + add + 1);
    if (grown == NULL) {
        return false;
    }
    memcpy(grown + old_len, text, add + 1);
    m->text = grown;
    return true;
}

void chat_clear(Chat *chat)
{
    for (size_t i = 0; i < chat->len; i++) {
        msg_free_fields(&chat->msgs[i]);
    }
    chat->len = 0;
}

void chat_free(Chat *chat)
{
    chat_clear(chat);
    free(chat->msgs);
    chat->msgs = NULL;
    chat->cap = 0;
}

int chat_set_tool_calls(Chat *chat, ChatToolCall *calls, size_t len)
{
    if (chat == NULL || chat->len == 0) {
        return -1;
    }
    ChatMessage *m = &chat->msgs[chat->len - 1];
    if (m->role != CHAT_ROLE_ASSISTANT) {
        return -1; /* nur die antwort des modells traegt calls */
    }
    /* vorhandene calls (sollte keine geben) freigeben */
    for (size_t i = 0; i < m->tool_calls_len; i++) {
        free(m->tool_calls[i].id);
        free(m->tool_calls[i].name);
        free(m->tool_calls[i].arguments);
    }
    free(m->tool_calls);
    m->tool_calls = calls;
    m->tool_calls_len = len;
    return 0;
}

/* ------------------------------------------------------------------ */
/* input -> text: die zeilen des eingabefelds (input.c) mit '\n'     */
/* joinen. eine eingabe, in der jede zeile leer ist, gilt als leer. */
/* NULL-zeilen duerfen laut input-invariante nicht vorkommen, wir   */
/* gehen trotzdem defensiv damit um (zaehlen als leer).             */
/* ------------------------------------------------------------------ */
char *chat_flatten_input(const Input *in)
{
    /* total = summe der laengen + (count - 1) trenner */
    size_t total = 0;
    bool any_text = false;
    for (size_t i = 0; i < in->count; i++) {
        size_t n = (in->lines[i] != NULL) ? strlen(in->lines[i]) : 0;
        total += n;
        if (n > 0) {
            any_text = true;
        }
    }
    if (!any_text) {
        return NULL;
    }
    if (in->count > SIZE_MAX - total) {
        return NULL; /* overflow: mehr zeilen als speicheradresse */
    }
    total += in->count; /* trenner + terminierung */

    char *buf = malloc(total);
    if (buf == NULL) {
        return NULL;
    }
    char *p = buf;
    for (size_t i = 0; i < in->count; i++) {
        if (i > 0) {
            *p++ = '\n';
        }
        if (in->lines[i] != NULL) {
            size_t n = strlen(in->lines[i]);
            memcpy(p, in->lines[i], n);
            p += n;
        }
    }
    *p = '\0';
    return buf;
}
/* ------------------------------------------------------------------ */
/* word-wrap                                                          */
/*                                                                    */
/* ein codepoint zaehlt als 1 sichtbare zelle. eine wcwidth-tabelle   */
/* (CJK = 2 zellen, zero-width-combining) gibt es absichtlich nicht: */
/* chat-text ist fast immer latein/emoji-mix und der fehler betraegt */
/* nur die umbruchposition um ein paar zellen.                      */
/* ------------------------------------------------------------------ */

/* ein codepoint ab s: byte-laenge nach *bytes. defekte sequences
 * (lead-byte ohne folge-bytes) zaehlen als einzelbyte, gelesen wird
 * nie ueber den string-terminator hinaus. */
static size_t utf8_step(const char *s, size_t *bytes)
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
            n = 1; /* abgebrochene sequenz: als einzelbyte zaehlen */
            break;
        }
    }
    *bytes = n;
    return n;
}

/* eine zeile in die tabelle schreiben (bzw. nur mitzaehlen, wenn
 * out NULL ist oder die arena voll) */
static void wrap_emit(ChatRole role, size_t msg, size_t off, size_t len,
                      bool first, bool lstart, int tool, ChatLine *out,
                      size_t out_max, size_t *count)
{
    if (out != NULL && *count < out_max) {
        out[*count].role = role;
        out[*count].msg = msg;
        out[*count].off = off;
        out[*count].len = len;
        out[*count].first = first;
        out[*count].lstart = lstart;
        out[*count].tool = tool;
        out[*count].blk_start = 0;
        out[*count].blk_end = 0;
    }
    (*count)++;
}

/* tabelle: ausgerichtete darstellung (md_table_display) und deren
 * zeilen emittieren. die ChatLines zeigen mit tool==-2 auf den
 * darstellungs-string; blk_start/blk_end verweisen auf die tabelle
 * im ORIGINAL-text, damit der renderer sie (fuer die farbe)
 * identifizieren kann. */
static void wrap_table(ChatRole role, size_t msg, const char *text,
                       size_t start, size_t end, int full_width,
                       ChatLine *out, size_t out_max, size_t *count)
{
    char *disp = md_table_display(text, start, end, full_width);
    if (disp == NULL) {
        return; /* keine tabelle (defensiv) */
    }
    size_t dl = strlen(disp);
    size_t off = 0;
    bool first = true;
    while (off < dl) {
        size_t e = off;
        while (e < dl && disp[e] != '\n') {
            e++;
        }
        wrap_emit(role, msg, off, e - off, first, first, -2, out, out_max,
                  count);
        /* blk-grenzen nachtragen (wrap_emit kennt sie nicht) */
        if (out != NULL && *count <= out_max) {
            out[*count - 1].blk_start = start;
            out[*count - 1].blk_end = end;
        }
        first = false;
        off = e + 1;
    }
    free(disp);
}

/* hex-wert einer ziffer, -1 bei allem anderen */
static int hex_val(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

/* "\uXXXX" an s (zeigt auf den backslash) parsen. rueckgabe: der
 * codepoint, -1 wenn kein gueltiges escape dasteht. *consumed = die
 * byte-laenge des escapes (6, oder 12 bei einem surrogate-paar).
 * liest nie ueber den string-terminator hinaus: hex_val('\0')
 * liefert -1 und bricht ab. */
static long parse_unicode_escape(const char *s, size_t *consumed)
{
    if (s[0] != '\\' || s[1] != 'u') {
        return -1;
    }
    long cp = 0;
    for (int k = 0; k < 4; k++) {
        int v = hex_val(s[2 + k]);
        if (v < 0) {
            return -1;
        }
        cp = (cp * 16) + v;
    }
    *consumed = 6;
    if (cp >= 0xD800 && cp <= 0xDBFF) {
        /* high-surrogate: nur zusammen mit einem low-paar ein
         * gueltiges zeichen (z.B. emoji) */
        if (s[6] == '\\' && s[7] == 'u') {
            long lo = 0;
            bool ok = true;
            for (int k = 0; k < 4; k++) {
                int v = hex_val(s[8 + k]);
                if (v < 0) {
                    ok = false;
                    break;
                }
                lo = (lo * 16) + v;
            }
            if (ok && lo >= 0xDC00 && lo <= 0xDFFF) {
                *consumed = 12;
                return 0x10000 + ((cp - 0xD800) * 0x400) + (lo - 0xDC00);
            }
        }
        return -1; /* unpaariges surrogate: original stehen lassen */
    }
    if (cp >= 0xDC00 && cp <= 0xDFFF) {
        return -1; /* lone low-surrogate */
    }
    return cp;
}

/* codepoint als utf-8 nach dst, byte-laenge zurueck (1..4) */
static size_t utf8_encode(long cp, char *dst)
{
    if (cp < 0x80) {
        dst[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        dst[0] = (char)(0xC0 | (cp >> 6));
        dst[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        dst[0] = (char)(0xE0 | (cp >> 12));
        dst[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        dst[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    dst[0] = (char)(0xF0 | (cp >> 18));
    dst[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    dst[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    dst[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

/* json-unicode-escapes (\uXXXX) in s IN-PLACE dekodieren: manche
 * modelle schicken "&" oder umlaute als \u0026/\u00e4 – in der
 * anzeige soll das lesbar sein. nur die DARSTELLUNG wird beruehrt,
 * die rohen argumente (api-round-trip, session-log) bleiben wie
 * sie sind. steuerzeichen-escapes (\u000a) bleiben stehen: ein
 * echter zeilenumbruch mitten in der darstellungs-zeile wuerde
 * das rendering kaputt machen. kaputte escapes bleiben literal. */
static void decode_unicode_escapes(char *s)
{
    size_t r = 0; /* lese-position */
    size_t w = 0; /* schreib-position */
    while (s[r] != '\0') {
        if (s[r] == '\\') {
            if (s[r + 1] == '\\') {
                /* doppelter backslash: im rohen json ein ESCAPTER
                 * backslash – das folgende \u0026 gehoert nicht
                 * uns. beide zeichen unangetastet lassen */
                s[w++] = s[r++];
                s[w++] = s[r++];
                continue;
            }
            size_t consumed = 0;
            long cp = parse_unicode_escape(s + r, &consumed);
            if (cp > 0x1F && cp != 0x7F) {
                /* invariante: w <= r, und dekodieren liest mindestens
                 * 6 byte (bzw. 12), schreibt aber hoechstens 4 – der
                 * schreibzeiger kann den lesezeiger nie ueberholen,
                 * jedes geschriebene byte bleibt im string */
                char enc[4];
                size_t n = utf8_encode(cp, enc);
                for (size_t k = 0; k < n; k++) {
                    s[w++] = enc[k];
                }
                r += consumed;
                continue;
            }
        }
        s[w++] = s[r++];
    }
    s[w] = '\0';
}

char *chat_tool_display(const ChatToolCall *call)
{
    static const char arrow[] = "\xE2\x86\x92 "; /* utf-8 pfeil + space */
    static const size_t arrow_len = sizeof arrow - 1;
    const char *name = (call->name != NULL) ? call->name : "?";
    const char *args = (call->arguments != NULL) ? call->arguments : "";
    size_t nlen = strlen(name);
    size_t alen = strlen(args);
    if (nlen > SIZE_MAX - alen || nlen + alen > SIZE_MAX - arrow_len - 2) {
        die("out of memory"); /* laengen-ueberlauf: rein theoretisch */
    }
    char *s = malloc(arrow_len + nlen + 1 + alen + 1 + 1);
    if (s == NULL) {
        die("out of memory");
    }
    size_t pos = 0;
    memcpy(s + pos, arrow, arrow_len);
    pos += arrow_len;
    memcpy(s + pos, name, nlen);
    pos += nlen;
    s[pos++] = '(';
    memcpy(s + pos, args, alen);
    pos += alen;
    s[pos++] = ')';
    s[pos] = '\0';
    /* anzeige entschaerfen: \u0026 -> & usw. (nur darstellung!) */
    decode_unicode_escapes(s);
    return s;
}

/* tool-call in darstellungs-zeilen zerlegen: die erste beginnt mit
 * pfeil und name am linken rand, fortsetzungen ruecken um
 * TOOL_INDENT_W ein. off/len der emittierten ChatLines verweisen
 * auf den darstellungs-string (chat_tool_display) – genau den
 * baut sich row_tool_call zum zeichnen wieder auf. umbruch wie beim
 * nachrichten-text: am letzten leerzeichen, ueberlange worte hart
 * an der breite. */
static void wrap_tool_call(ChatRole role, size_t msg, int tool,
                           const ChatToolCall *call, int width, ChatLine *out,
                           size_t out_max, size_t *count)
{
    char *s = chat_tool_display(call);
    if (s == NULL) {
        die("out of memory");
    }

    int line_w = width;
    if (line_w < 1) {
        line_w = 1;
    }
    bool first_line = true;
    size_t off = 0;        /* byte-offset des zeilenanfangs in s */
    size_t brk = SIZE_MAX; /* letztes passendes leerzeichen */
    int cells = 0;         /* sichtbare zellen seit zeilenanfang */
    size_t i = 0;
    while (s[i] != '\0') {
        if (s[i] == ' ') {
            brk = i;
        }
        size_t blen;
        (void)utf8_step(s + i, &blen);
        if (cells + 1 > line_w) {
            size_t next;
            if (brk != SIZE_MAX && brk > off) {
                wrap_emit(role, msg, off, brk - off, first_line, first_line,
                          tool, out, out_max, count);
                next = brk + 1;
            } else {
                wrap_emit(role, msg, off, i - off, first_line, first_line,
                          tool, out, out_max, count);
                next = i;
            }
            first_line = false;
            line_w = width - TOOL_INDENT_W;
            if (line_w < 1) {
                line_w = 1;
            }
            i = next;
            off = next;
            brk = SIZE_MAX;
            cells = 0;
            continue;
        }
        cells++;
        i += blen;
    }
    /* rest; auch ein kurzer aufruf ("-> name()") ist genau eine
     * zeile. endete der text exakt am letzten umbruch, bleibt
     * nichts uebrig */
    if (off < i || first_line) {
        wrap_emit(role, msg, off, i - off, first_line, first_line, tool, out,
                  out_max, count);
    }
    free(s);
}

size_t chat_wrap(const Chat *chat, int width, ChatLine *out, size_t out_max)
{
    if (chat == NULL || width < 1) {
        return 0;
    }
    size_t w = (size_t)width;
    size_t count = 0;

    for (size_t mi = 0; mi < chat->len; mi++) {
        const char *text = chat->msgs[mi].text;
        if (text == NULL) {
            continue;
        }
        ChatRole role = chat->msgs[mi].role;

        /* markdown-bloecke NUR fuer ki-antworten: tabellen werden
         * als ausgerichtete darstellung emittiert (volle breite),
         * fences als text (row_msg tokenisiert sie). der scan ist
         * billig (ein durchlauf) und wird je frame neu gemacht. */
        MdBlocks blks;
        bool has_blocks = false;
        if (role == CHAT_ROLE_ASSISTANT) {
            md_block_scan(text, &blks);
            has_blocks = (blks.n > 0);
        }

        bool first = true;     /* erste zeile dieser nachricht */
        bool lstart = true;    /* zeile beginnt am originalen \n-anfang */
        size_t off = 0;        /* byte-offset des zeilenanfangs */
        size_t cells = 0;      /* zellen seit dem zeilenanfang */
        size_t brk = SIZE_MAX; /* offset des letzten passenden leerzeichens */
        size_t i = 0;

        while (text[i] != '\0') {
            /* tabelle? an jedem originalen zeilenanfang (lstart)
             * pruefen: beginnt hier ein tabellen-block, wird er
             * als GANZES als darstellung emittiert und danach
             * hinter dem block weitergescannt */
            if (has_blocks && off == i) {
                const MdBlock *tb = md_block_at(&blks, i);
                if (tb != NULL && tb->kind == MD_BLK_TABLE) {
                    wrap_table(role, mi, text, tb->start, tb->end, width, out,
                               out_max, &count);
                    first = false;
                    lstart = true;
                    i = tb->end;
                    off = i;
                    cells = 0;
                    brk = SIZE_MAX;
                    continue;
                }
            }
            if (text[i] == '\r') {
                /* CR sollte nie hier ankommen (chat_append filtert);
                 * defensiv als zeilenumbruch werten statt es im text
                 * zu belassen */
                wrap_emit(role, mi, off, i - off, first, lstart, -1, out,
                          out_max, &count);
                first = false;
                lstart = true; /* neue logische zeile */
                i++;
                off = i;
                cells = 0;
                brk = SIZE_MAX;
                continue;
            }
            if (text[i] == '\n') { /* erzwungener umbruch */
                wrap_emit(role, mi, off, i - off, first, lstart, -1, out,
                          out_max, &count);
                first = false;
                lstart = true; /* neue logische zeile */
                i++;
                off = i;
                cells = 0;
                brk = SIZE_MAX;
                continue;
            }
            if (text[i] == ' ') {
                brk = i;
            }
            size_t blen;
            (void)utf8_step(text + i, &blen);
            if (cells + 1 > w) {
                if (brk != SIZE_MAX && brk > off) {
                    /* am letzten leerzeichen umbrechen; ein space-
                     * run dahinter faellt ganz weg */
                    wrap_emit(role, mi, off, brk - off, first, lstart, -1, out,
                              out_max, &count);
                    first = false;
                    lstart = false; /* umbruch, kein original-\n */
                    size_t next = brk + 1;
                    while (text[next] == ' ') {
                        next++;
                    }
                    i = next;
                    off = next;
                } else {
                    /* kein umbruchpunkt in der zeile (ueberlanges
                     * wort): hart an der breite brechen */
                    wrap_emit(role, mi, off, i - off, first, lstart, -1, out,
                              out_max, &count);
                    first = false;
                    lstart = false; /* umbruch, kein original-\n */
                    off = i; /* i bleibt: das ueberlaufende zeichen */
                             /* startet die naechste zeile         */
                }
                cells = 0;
                brk = SIZE_MAX;
                continue;
            }
            cells++;
            i += blen;
        }

        /* rest der nachricht. endet der text auf '\n', entsteht dahinter
         * KEINE leere zeile (off == i); eine nachricht ohne jede zeile
         * (leerer text, z.B. streaming-platzhalter) bekommt genau eine
         * leere erste zeile, damit der label alleine steht. */
        if (off < i || first) {
            wrap_emit(role, mi, off, i - off, first, lstart, -1, out, out_max,
                      &count);
        }

        /* tool-calls der nachricht: darstellungs-zeilen nach dem
         * text. uebergrosse aufrufe – z.B. bash mit sehr langen
         * parametern – brechen am zeilenrand um, fortsetzungen
         * ruecken um TOOL_INDENT_W ein */
        for (size_t t = 0; t < chat->msgs[mi].tool_calls_len; t++) {
            wrap_tool_call(role, mi, (int)t, &chat->msgs[mi].tool_calls[t],
                           width, out, out_max, &count);
        }
    }
    return count;
}
