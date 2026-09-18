#define _POSIX_C_SOURCE 200809L // NOLINT(bugprone-reserved-identifier)

#include "draw.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "chat.h"
#include "command.h"
#include "debug.h"
#include "input.h"
#include "markdown.h"
#include "send.h"
#include "session.h"
#include "settings.h"
#include "theme.h"
#include "utils.h"

static const char GLYPH_EM_DASH[] = "\xE2\x80\x94";
static const char GLYPH_BLOCK[] = "\xE2\x96\x88";

/* nachrichten starten direkt am linken rand (kein label mehr),
 * haben aber links und rechts je EIN leerzeichen rand zum rest
 * des bildes. die wrap-breite rechnet das ab: text_w ist die
 * nutzbare breite zwischen den paddings. */
#define MSG_PAD_W 1

/* kein eingabe-praefix mehr: der text beginnt bei spalte 0 –
 * diese spalte bleibt fuer den cursor-block am zeilenende frei */
#define INPUT_PREFIX_W 0

/* statuszeilen am unteren rand des docks: 2 zeilen brechen bei
 * schmalen terminals um, jede hoechstens 3 bildschirmzeilen hoch
 * (dock_build rechnet mit den echten zahlen). STATUS_H ist nur der
 * alt-last-default, der nie mehr direkt benutzt wird. */
#define STATUS_H 2

/* maximalzeilen des docks: quit + cmds + eingabe-rahmen + dialog +
 * status – der dock klemmt ohnehin am terminal */
#define DOCK_ROWS_MAX 96

/* die KLEMME: eine nachricht, die (nach wrap, inkl. der von \n
 * erzeugten echten zeilen) mehr als 75% der fensterhoehe belegt,
 * verliert ihre UEBERSCHUSS-ZEILEN unten – sichtbar bleiben die
 * ersten, dahinter der graue hinweis "... und X weitere zeilen",
 * und als letzte zeile die letzte der nachricht (der exit-code
 * eines tool-outputs haengt dort, das stream-ende waechst dort).
 * die klemme wird JE FRAME neu berechnet – ein resize wirkt
 * sofort; was schon committet ist, bleibt unveraendert im
 * scrollback (nur neue frames klemmen den neuen schwanz). */
#define MSG_MAX_PCT 75

/* ------------------------------------------------------------------ */
/* ausgabe-primitive: alles laeuft ueber g_out (stdout oder ein      */
/* test-FILE*). der frame-buffer gilt je ZEILE, nicht mehr wie im   */
/* vollbild fuer den ganzen bildschirm.                              */
/* ------------------------------------------------------------------ */

static FILE *g_out = NULL; /* NULL = stdout (default) */

static FILE *out(void)
{
    return (g_out != NULL) ? g_out : stdout;
}

typedef struct {
    char *buf;
    size_t pos;
} Frame;

typedef struct {
    Frame *f;
    int width; /* zellen bis zum rand des hauptbereichs */
    int cells; /* bereits geschriebene sichtbare zellen */
} Row;

static char *g_row_buf = NULL;
static size_t g_row_buf_cap = 0;
static Frame g_frame;
static Row g_row;

/* ------------------------------------------------------------------ */
/* frame-sammler: der KOMPLETTE frame (erase, content, live, dock)   */
/* wird hier gesammelt und mit einem einzigen fwrite+fflush ausge-   */
/* geben. stdout am terminal ist zeilen-gepuffert – ohne den sammler */
/* landete jede zeile einzeln und der benutzer sah das geraeumte     */
/* dock als flackern (erase erst, zeichnen danach, dazwischen ein    */
/* anstrich des leeren zustands). ein write pro frame = kein        */
/* zwischenzustand auf dem bildschirm.                               */
/* ------------------------------------------------------------------ */
static char *g_flush_buf = NULL;
static size_t g_flush_cap = 0;
static size_t g_flush_pos = 0;

static void flush_reserve(size_t need)
{
    if (g_flush_cap >= need) {
        return;
    }
    size_t cap = (g_flush_cap > 0) ? g_flush_cap : 8192;
    while (cap < need) {
        if (cap > SIZE_MAX / 2) {
            die("out of memory");
        }
        cap *= 2;
    }
    char *grown = realloc(g_flush_buf, cap);
    if (grown == NULL) {
        die("out of memory");
    }
    g_flush_buf = grown;
    g_flush_cap = cap;
}

static void flush_put(const char *s, size_t n)
{
    flush_reserve(g_flush_pos + n);
    memcpy(g_flush_buf + g_flush_pos, s, n);
    g_flush_pos += n;
}

static void flush_puts(const char *s)
{
    flush_put(s, strlen(s));
}

static void flush_up(int n)
{
    char buf[16];
    (void)snprintf(buf, sizeof buf, "\x1b[%dA", n);
    flush_puts(buf);
}

static void flush_down(int n)
{
    char buf[16];
    (void)snprintf(buf, sizeof buf, "\x1b[%dB", n);
    flush_puts(buf);
}

/* den gesammelten frame als EIN write rauslassen */
static void flush_out(void)
{
    fwrite(g_flush_buf, 1, g_flush_pos, out());
    fflush(out());
    g_flush_pos = 0;
}

/* ------------------------------------------------------------------ */
/* zeilen-renderer: fuellen die aktuelle zeile (g_row)               */
/* ------------------------------------------------------------------ */

static void row_pad(Row *r);
static int row_putc(Row *r, char c);
static void row_glyph(Row *r, const char *sym);
static void row_sgr(Row *r, const char *seq);
static void row_putn(Row *r, const char *s, int n);
static void row_puts(Row *r, const char *s);
static void row_border(Row *r);
static void row_input(Row *r, const Input *in, int width, size_t idx);
static void row_dlg_search(Row *r, const char *title, const char *search);
static void row_no_dlg_match(Row *r, const char *search);
static void row_model(Row *r, const Config *cfg, int idx, const char *search,
                      bool selected, int id_col);
static void row_setting(Row *r, const Config *cfg, int idx, const char *search,
                        bool selected, int id_col);
static void row_theme_opt(Row *r, int idx, const char *search, bool selected,
                          bool active, int id_col);
static void row_session(Row *r, const SessionInfo *si, const char *search,
                        bool selected, bool active, int id_col, int name_col);
static void row_command(Row *r, const Command *cmd, const char *prefix);
static void row_no_match(Row *r, const char *prefix);
/* ------------------------------------------------------------------ */
/* statuszeilen: der inhalt ist eine segment-liste, die bei schmalen */
/* terminals umbricht. trenner ("  \xC2\xB7  ") kleben am segment,      */
/* das ihnen folgt – der umbruch landet zwischen inhalten. jede     */
/* der beiden zeilen belegt 1..STATUS_MAX_LINES bildschirmzeilen,   */
/* mehr nicht (sehr schmale terminals schneiden dann ab).            */
/* ------------------------------------------------------------------ */
#define STATUS_MAX_LINES 3
#define STATUS_SEG_MAX   24
#define STATUS_LABEL_W   9 /* "model   " / "tokens  " + leerzeichen */

/* segment-text inline: die zahlen/dauern entstehen in lokalen
 * puffern der seg-builder – ein pointer darauf waere nach deren
 * return verwaiset (ASan: stack-use-after-return). deshalb wird
 * der text HIER hineinkopiert. */
#define STATUS_SEG_TEXT 40

typedef struct {
    char text[STATUS_SEG_TEXT]; /* utf-8, '\0'-terminiert */
    const char *sgr;            /* farbsequenz oder NULL = dim */
    bool sep_before;            /* "  \xC2\xB7  " gehoert zu diesem segment */
} StatusSeg;

typedef struct {
    StatusSeg segs[STATUS_SEG_MAX];
    int n;
} StatusLine;

static void st_add(StatusLine *l, const char *text, const char *sgr,
                   bool sep_before);
static int st_fill(const StatusLine *l, int *idx, int width, int *cell,
                   int line, int last, bool dry, Row *r);
static int st_lines(const StatusLine *l, int width);
static void status_model_segs(StatusLine *l, const Config *cfg);
static void status_token_segs(StatusLine *l, const AppState *st);
static void row_status_model(Row *r, const Config *cfg, int sub);
static void row_status_tokens(Row *r, const AppState *st, int sub);
static void row_quit(Row *r);
static void row_msg(Row *r, const ChatLine *ln, const char *text);
static void row_tool_call(Row *r, const ChatToolCall *call, const ChatLine *ln);
static void row_inline(Row *r, const char *text, size_t off, size_t len,
                       bool assistant);
static void row_busy_border(Row *r, long long busy_ms);
static void row_tool_spinner(Row *r, long long busy_ms);
static int exit_marker_of(const char *text, const ChatLine *ln);
static void fmt_dur(long long ms, char *buf, size_t sz);
static void fmt_when(long long ms, char *buf, size_t sz);

static void row_pad(Row *r)
{
    while (r->cells < r->width && row_putc(r, ' ') != 0) {
    }
}

/* byte-laenge des utf-8-zeichens, das bei s[0] beginnt. defekte
 * folgen gelten als einzelbyte, ueber den terminator wird nie
 * gelesen (s[0] != '\0' ist am aufrufort gesichert). gleiche
 * logik wie wrap_step (input.c) und utf8_step (chat.c) */
static size_t row_utf8_step(const char *s)
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

/* ein zeichen aus s uebernehmen, soweit es in die zeile passt
 * (defekte folgen gelten als einzelbyte). zaehlt CODEPOINTS als
 * zellen – ein umlaut/emoji ist EINE sichtbare zelle, nicht
 * seine byte-laenge. genau so zaehlen die umbruch-planer
 * (input.c soft-wrap, chat_wrap): nur wenn beide gleich zaehlen,
 * bleibt nichts am zeilenrand haengen. rueckgabe: 1 = zeichen
 * steht, 0 = kein platz mehr. */
static int row_putc(Row *r, char c)
{
    if (r->cells >= r->width) {
        return 0;
    }
    r->f->buf[r->f->pos++] = c;
    r->cells++;
    return 1;
}

static void row_glyph(Row *r, const char *sym)
{
    if (r->cells >= r->width) {
        return;
    }
    size_t len = strlen(sym);
    memcpy(r->f->buf + r->f->pos, sym, len);
    r->f->pos += len;
    r->cells++;
}

/* ansi-sequenz (z.B. farb-wechsel): unsichtbar, zaehlt nicht */
static void row_sgr(Row *r, const char *seq)
{
    while (*seq != '\0') {
        r->f->buf[r->f->pos++] = *seq++;
    }
}

/* einen codepoint aus s schreiben, wenn EINE weitere zelle frei
 * ist. defekte folgen gelten als einzelbyte. rueckgabe: 1 = er
 * steht, 0 = kein platz (string-ende prueft der aufrufer) */
static int row_put_cp(Row *r, const char *s)
{
    size_t clen = row_utf8_step(s);
    if ((size_t)r->cells + 1U > (size_t)r->width) {
        return 0;
    }
    memcpy(r->f->buf + r->f->pos, s, clen);
    r->f->pos += clen;
    r->cells++; /* EIN codepoint = EINE zelle */
    return 1;
}

/* die naechsten n byte von s: n<=0 oder string-ende geben nichts
 * aus. die ZELLE wird je codepoint gezaehlt (ein umlaut/emoji =
 * EINE zelle), die bytes landen komplett im frame – sonst waere
 * ein umlaut 2 zellen breit und der renderer wuerde vom
 * umbruch-plan abweichen (genau der bug, der die letzten zeichen
 * vom rand fallen liess). passt das zeichen nicht mehr ganz in
 * die zeile, bleibt es ganz draussen – halbe utf-8-sequenzen
 * werden nie ausgegeben. */
static void row_putn(Row *r, const char *s, int n)
{
    size_t left = (n > 0) ? (size_t)n : 0;
    while (left > 0 && s[0] != '\0') {
        size_t clen = row_utf8_step(s);
        if (clen > left) {
            break; /* rest gehoert einem halben zeichen: weg */
        }
        if (row_put_cp(r, s) == 0) {
            return; /* kein platz mehr fuer EINE weitere zelle */
        }
        s += clen;
        left -= clen;
    }
}

static void row_puts(Row *r, const char *s)
{
    while (s[0] != '\0' && row_put_cp(r, s) != 0) {
        s += row_utf8_step(s);
    }
}

static void row_border(Row *r)
{
    while (r->cells < r->width) {
        row_glyph(r, GLYPH_EM_DASH);
    }
}

/* eine BILDSCHIRMzeile des eingabefelds. idx zaehlt ueber alle
 * umgebrochenen zeilen hinweg, nicht ueber die logischen. */
static void row_input(Row *r, const Input *in, int width, size_t idx)
{
    size_t line = 0;
    size_t off = 0;
    size_t len = 0;
    if (!input_screen_row(in, width, idx, &line, &off, &len)) {
        return; /* feld groesser als der text: leerzeile */
    }
    /* KEIN prompt-zeichen: der benutzer tippt direkt am linken
     * rand, der cursor-block steht hinter dem text */
    const char *text = in->lines[line] + off;

    size_t crow = 0;
    input_cursor_screen(in, width, &crow, NULL);
    if (crow != idx) {
        row_putn(r, text, (int)len);
        return;
    }

    size_t cur = (in->cursor > off) ? in->cursor - off : 0;
    if (cur > len) {
        cur = len;
    }
    row_putn(r, text, (int)cur);
    row_glyph(r, GLYPH_BLOCK);
    row_putn(r, text + cur, (int)(len - cur));
}

static void row_dlg_search(Row *r, const char *title, const char *search)
{
    row_puts(r, " ");
    row_puts(r, title);
    row_puts(r, ": ");
    row_puts(r, search);
    row_glyph(r, GLYPH_BLOCK); /* cursor hinter dem suchtext */
}

static void row_no_dlg_match(Row *r, const char *search)
{
    /* 96: 17 fixe zeichen + search (max 63) + '\0' */
    char line[96];
    snprintf(line, sizeof line, "   kein eintrag: %s", search);
    row_puts(r, line);
}

static void row_model(Row *r, const Config *cfg, int idx, const char *search,
                      bool selected, int id_col)
{
    const char *url = "";
    const Model *m = model_at(cfg, idx, &url);
    if (m == NULL || m->id == NULL) {
        return;
    }
    const Theme *theme = theme_current();

    if (selected) {
        row_sgr(r, theme->match);
        row_puts(r, " > ");
        row_sgr(r, theme->reset);
    } else {
        row_puts(r, "   ");
    }

    int slen = (int)strlen(search);
    row_sgr(r, theme->match);
    row_putn(r, m->id, slen); /* getippter anteil: suchfarbe */
    row_sgr(r, theme->reset);
    row_putn(r, m->id + slen, (int)strlen(m->id) - slen);

    while (r->cells < 3 + id_col + 2) {
        row_putc(r, ' ');
    }
    row_puts(r, (url != NULL) ? url : "");
    if (m->reasoning) {
        row_puts(r, "  (reasoning)");
    }
}

static void row_setting(Row *r, const Config *cfg, int idx, const char *search,
                        bool selected, int id_col)
{
    const Theme *theme = theme_current();
    const char *name = SETTING_NAMES[idx];

    if (selected) {
        row_sgr(r, theme->match);
        row_puts(r, " > ");
        row_sgr(r, theme->reset);
    } else {
        row_puts(r, "   ");
    }

    int slen = (int)strlen(search);
    row_sgr(r, theme->match);
    row_putn(r, name, slen);
    row_sgr(r, theme->reset);
    row_putn(r, name + slen, (int)strlen(name) - slen);

    while (r->cells < 3 + id_col + 2) {
        row_putc(r, ' ');
    }

    switch ((SettingId)idx) {
    case SET_THEME:
        row_puts(r, (cfg->theme != NULL) ? cfg->theme : "auto");
        break;
    case SET_CONFIRM_QUIT:
        row_puts(r, on_off(cfg->confirm_quit));
        break;
    case SET_COUNT:
        break;
    }
}

static void row_theme_opt(Row *r, int idx, const char *search, bool selected,
                          bool active, int id_col)
{
    const Theme *theme = theme_current();
    const char *name = theme_option_name(idx);
    if (name == NULL) {
        return;
    }

    if (selected) {
        row_sgr(r, theme->match);
        row_puts(r, " > ");
        row_sgr(r, theme->reset);
    } else {
        row_puts(r, "   ");
    }

    int slen = (int)strlen(search);
    row_sgr(r, theme->match);
    row_putn(r, name, slen);
    row_sgr(r, theme->reset);
    row_putn(r, name + slen, (int)strlen(name) - slen);

    while (r->cells < 3 + id_col + 2) {
        row_putc(r, ' ');
    }
    if (active) {
        row_puts(r, "(active)");
    }
}

/* unix-ms -> "dd.mm hh:mm" (bzw. mit jahr, wenn es aelter ist) */
static void fmt_when(long long ms, char *buf, size_t sz)
{
    buf[0] = '?';
    buf[1] = '\0';
    if (ms <= 0) {
        return;
    }
    time_t t = (time_t)(ms / 1000);
    struct tm tmv;
    if (localtime_r(&t, &tmv) == NULL) {
        return;
    }
    time_t now = time(NULL);
    struct tm nowv;
    (void)localtime_r(&now, &nowv);
    if (tmv.tm_year == nowv.tm_year) {
        (void)strftime(buf, sz, "%d.%m %H:%M", &tmv);
    } else {
        (void)strftime(buf, sz, "%d.%m.%Y", &tmv);
    }
}

/* eintrag der session-liste (resume-dialog) */
static void row_session(Row *r, const SessionInfo *si, const char *search,
                        bool selected, bool active, int id_col, int name_col)
{
    const Theme *theme = theme_current();

    const char *name = "(ohne nachrichten)";
    if (si->name != NULL) {
        name = si->name;
    } else if (si->preview != NULL) {
        name = si->preview;
    }

    if (selected) {
        row_sgr(r, theme->match);
        row_puts(r, " > ");
        row_sgr(r, theme->reset);
    } else {
        row_puts(r, "   ");
    }

    int slen = (int)strlen(search);
    /* name auf die spaltenbreite kappen: id/datum/nachrichten-zahl
     * rutschen auf schmalen terminals nie vom rand */
    int show = (int)strlen(name);
    if (show > name_col) {
        show = name_col;
    }
    int hl = slen; /* getippter anteil: suchfarbe */
    if (hl > show) {
        hl = show;
    }
    row_sgr(r, theme->match);
    row_putn(r, name, hl);
    row_sgr(r, theme->reset);
    row_putn(r, name + hl, show - hl);

    if (active) {
        row_sgr(r, theme_role(THEME_ROLE_DIM));
        row_puts(r, " (aktiv)");
        row_sgr(r, THEME_ROLE_RESET);
    }

    int col = 3 + name_col + 2;
    if (active) {
        col += 8;
    }
    while (r->cells < col) {
        row_putc(r, ' ');
    }

    row_sgr(r, theme_role(THEME_ROLE_DIM));
    row_putn(r, si->id, slen);
    row_sgr(r, THEME_ROLE_RESET);
    row_putn(r, si->id + slen, (int)strlen(si->id) - slen);
    while (r->cells < col + id_col + 2) {
        row_putc(r, ' ');
    }

    char when[24];
    fmt_when(si->updated_at, when, sizeof when);
    row_sgr(r, theme_role(THEME_ROLE_DIM));
    row_puts(r, when);
    row_puts(r, "  \xC2\xB7  ");
    {
        char buf[32];
        (void)snprintf(buf, sizeof buf, "%zu %s", si->messages,
                       (si->messages == 1) ? "nachricht" : "nachrichten");
        row_puts(r, buf);
    }
    row_sgr(r, THEME_ROLE_RESET);
}

static void row_command(Row *r, const Command *cmd, const char *prefix)
{
    const Theme *theme = theme_current();
    int plen = (int)strlen(prefix);

    row_puts(r, "  /");
    row_sgr(r, theme->match);
    row_putn(r, cmd->name, plen);
    row_sgr(r, theme->reset);
    row_putn(r, cmd->name + plen, (int)strlen(cmd->name) - plen);

    while (r->cells < 3 + cmd_name_col() + 2) {
        row_putc(r, ' ');
    }
    row_puts(r, cmd->desc);
}

static void row_no_match(Row *r, const char *prefix)
{
    char line[80];
    snprintf(line, sizeof line, "  kein befehl: /%s", prefix);
    row_puts(r, line);
}

/* token-zahlen kurz halten: 1234 -> "1.2k", 45678 -> "45k".
 * die _s-variante schreibt in einen puffer: die statuszeilen
 * sammeln ihre inhalte als segmente und brauchen strings */
static void put_count_s(char *buf, size_t sz, size_t n)
{
    if (n < 1000) {
        (void)snprintf(buf, sz, "%zu", n);
    } else if (n < 100000) {
        (void)snprintf(buf, sz, "%zu.%zuk", n / 1000, (n % 1000) / 100);
    } else {
        (void)snprintf(buf, sz, "%zuk", n / 1000);
    }
}

/* sichtbare zellen eines utf-8-strings (ein codepoint = 1 zelle,
 * wie ueberall im renderer: keine wcwidth-tabelle). nur fuer den
 * umbruch-entscheid der statuszeilen */
static int utf8_cells(const char *s)
{
    int cells = 0;
    while (*s != '\0') {
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
                n = 1; /* abgebrochene folge: einzelbyte */
                break;
            }
        }
        cells++;
        s += n;
    }
    return cells;
}

static void status_label(Row *r, const char *label)
{
    row_sgr(r, theme_role(THEME_ROLE_DIM));
    row_puts(r, " ");
    row_puts(r, label);
    row_sgr(r, THEME_ROLE_RESET);
}

/* ------------------------------------------------------------------ */
/* statuszeilen: segment-liste -> bildschirmzeilen                     */
/* ------------------------------------------------------------------ */

static void st_add(StatusLine *l, const char *text, const char *sgr,
                   bool sep_before)
{
    if (l->n >= STATUS_SEG_MAX || text == NULL || text[0] == '\0') {
        return;
    }
    (void)snprintf(l->segs[l->n].text, STATUS_SEG_TEXT, "%s", text);
    l->segs[l->n].sgr = sgr;
    l->segs[l->n].sep_before = sep_before;
    l->n++;
}

/* eine bildschirmzeile fuellen. *idx laeuft ueber die segmente,
 * rueckgabe = es wurde etwas platziert. dry = nur planen (fuer
 * zeilenzahl/ueberspringen), sonst schreibt r. breite = zellen AB
 * dem label (der caller druckt das label selbst).
 * line/last: vorherige zeilen brechen sauber um, auf der LETZTEN
 * erlaubten zeile (line == last) wird ein nicht mehr ganz passen-
 * des segment HART an der breite abgeschnitten statt ganz fal-
 * len gelassen – sonst faele z.B. der log-pfad bei schmalen
 * terminals weg, obwohl noch platz auf der zeile waere. */
static int st_fill(const StatusLine *l, int *idx, int width, int *cell,
                   int line, int last, bool dry, Row *r)
{
    bool wrote = false;
    while (*idx < l->n) {
        const StatusSeg *s = &l->segs[*idx];
        int need = utf8_cells(s->text) + (s->sep_before ? 5 : 0);
        bool fits = (*cell + need <= width);
        if (!fits && line != last) {
            break; /* passt nicht: rest auf die naechste zeile.
                    * nur auf der letzten erlaubten zeile wird
                    * abgeschnitten statt umgebrochen */
        }
        if (!dry) {
            const char *sgr =
                (s->sgr != NULL) ? s->sgr : theme_role(THEME_ROLE_DIM);
            if (s->sep_before) {
                /* trenner + segment teilen sich die farbe: der
                 * trenner ist immer DIM, das segment kann farbig
                 * sein (modell-id, session-name) – sgr einmal
                 * setzen reicht */
                if (sgr == theme_role(THEME_ROLE_DIM)) {
                    row_sgr(r, theme_role(THEME_ROLE_DIM));
                    row_puts(r, "  \xC2\xB7  ");
                    row_puts(r, s->text);
                    /* kein erneutes sgr: dieselbe farbe laeuft */
                    goto next;
                }
                row_sgr(r, theme_role(THEME_ROLE_DIM));
                row_puts(r, "  \xC2\xB7  ");
            }
            row_sgr(r, sgr);
            row_puts(r, s->text);
        }
    next:;
        *cell += need;
        if (*cell > width) {
            *cell = width;
        }
        wrote = true;
        (*idx)++;
        if (!fits) {
            break; /* hart abgeschnitten: zeile voll */
        }
    }
    /* die zeile endet IMMER im default-zustand: die alten renderer
     * schlossen mit THEME_ROLE_RESET ab. ohne das waere alles
     * nachfolgende (eingabefeld, dialoge) faint/grau – der zustand
     * wuerde bis zum naechsten sgr in der naechsten zeile hinein-
     * wirken (der leere rest einer zeile wird nie angestoert). */
    if (wrote && !dry) {
        row_sgr(r, THEME_ROLE_RESET);
    }
    return wrote;
}

/* wie viele bildschirmzeilen braucht die liste bei dieser breite
 * (max STATUS_MAX_LINES, mindest 1)? dock_build misst hieran, wie
 * viele dock-rows die zeile belegt */
static int st_lines(const StatusLine *l, int width)
{
    int idx = 0;
    int lines = 0;
    while (lines < STATUS_MAX_LINES) {
        int cell = 0;
        if (!st_fill(l, &idx, width, &cell, lines, STATUS_MAX_LINES - 1, true,
                     NULL)) {
            break;
        }
        lines++;
    }
    return (lines > 0) ? lines : 1;
}

/* segmente der model-zeile: modell-id, kontext-groesse, base-url.
 * von dock_build (zeilenzahl) und row_status_model (inhalt) benutzt */
static void status_model_segs(StatusLine *l, const Config *cfg)
{
    const Provider *provider = NULL;
    const Model *m = send_find_model(cfg, cfg->active_model, &provider);

    if (m == NULL || m->id == NULL) {
        st_add(l, "keins gewaehlt \xE2\x80\x93 /models",
               theme_role(THEME_ROLE_DIM), false);
        return;
    }
    st_add(l, m->id, theme_role(THEME_ROLE_ASSISTANT), false);
    char buf[40];
    if (m->context_window > 0) {
        char cnt[24];
        put_count_s(cnt, sizeof cnt, m->context_window);
        (void)snprintf(buf, sizeof buf, "%s kontext", cnt);
        st_add(l, buf, NULL, true);
    }
    if (provider != NULL && provider->base_url != NULL) {
        st_add(l, provider->base_url, NULL, true);
    }
}

static void row_status_model(Row *r, const Config *cfg, int sub)
{
    StatusLine l = {0};
    status_model_segs(&l, cfg);

    int width = r->width - STATUS_LABEL_W;
    if (width < 1) {
        width = 1;
    }
    int idx = 0;
    for (int i = 0; i < sub; i++) {
        int cell = 0;
        if (!st_fill(&l, &idx, width, &cell, i, STATUS_MAX_LINES - 1, true,
                     NULL)) {
            return; /* sub jenseits des inhalts: leer bleibt leer */
        }
    }
    status_label(r, (sub == 0) ? "model   " : "        ");
    int cell = 0;
    (void)st_fill(&l, &idx, width, &cell, sub, STATUS_MAX_LINES - 1, false, r);
}

/* segmente der token-zeile: aktueller kontext (mit cache-anteil),
 * arbeitszeit, session und – mit --debug – die log-datei
 * (dbg_path aktualisiert sich mit der session-id)
 *
 * der kontext ist die prompt-groesse des LETZTEN requests, nicht
 * eine kumulierte summe: frueher summierte jede runde den ganzen
 * verlauf nochmal ("679k gesendet"), obwohl der server den
 * prefix fast komplett aus seinem cache las – die zahl war
 * ungefaehr so aussagekraeftig wie ein tachometer, das die
 * gefahrenen meter aller tage addiert. der cache-anteil zeigt,
 * wieviel davon praktisch gratis war. */
static void status_token_segs(StatusLine *l, const AppState *st)
{
    char buf[80];
    {
        char cnt[24];
        put_count_s(cnt, sizeof cnt, (size_t)st->ctx.last_prompt);
        if (st->ctx.last_cached > 0) {
            char cch[24];
            put_count_s(cch, sizeof cch, (size_t)st->ctx.last_cached);
            (void)snprintf(buf, sizeof buf, "%s kontext (%s cached)", cnt, cch);
        } else {
            (void)snprintf(buf, sizeof buf, "%s kontext", cnt);
        }
        st_add(l, buf, NULL, false);
    }
    {
        char cnt[24];
        put_count_s(cnt, sizeof cnt, st->ctx.total_completion);
        (void)snprintf(buf, sizeof buf, "%s empfangen", cnt);
        st_add(l, buf, NULL, true);
    }

    if (st->worked_ms > 0) {
        char dur[24];
        fmt_dur(st->worked_ms, dur, sizeof dur);
        (void)snprintf(buf, sizeof buf, "arbeit %s", dur);
        st_add(l, buf, NULL, true);
    }
    if (st->session.active) {
        if (st->session.name != NULL) {
            /* name farbig, dahinter die id: der trenner zwischen
             * beiden gehoert zur id (sep_before) */
            st_add(l, "session", NULL, true);
            st_add(l, st->session.name, theme_role(THEME_ROLE_ASSISTANT),
                   false);
            st_add(l, st->session.id, NULL, true);
        } else {
            /* ohne namen: "session ID" als EIN segment (das alte
             * layout hatte beides in einem stueck) */
            (void)snprintf(buf, sizeof buf, "session %s", st->session.id);
            st_add(l, buf, NULL, true);
        }
    }
    if (dbg_active()) {
        char dbg[STATUS_SEG_TEXT];
        (void)snprintf(dbg, sizeof dbg, "debug mode, log file: %s", dbg_path());
        st_add(l, dbg, NULL, true);
    }
}

static void row_status_tokens(Row *r, const AppState *st, int sub)
{
    StatusLine l = {0};
    status_token_segs(&l, st);

    int width = r->width - STATUS_LABEL_W;
    if (width < 1) {
        width = 1;
    }
    int idx = 0;
    for (int i = 0; i < sub; i++) {
        int cell = 0;
        if (!st_fill(&l, &idx, width, &cell, i, STATUS_MAX_LINES - 1, true,
                     NULL)) {
            return;
        }
    }
    status_label(r, (sub == 0) ? "tokens  " : "        ");
    int cell = 0;
    (void)st_fill(&l, &idx, width, &cell, sub, STATUS_MAX_LINES - 1, false, r);
}

static void row_quit(Row *r)
{
    row_puts(r, "quit? ctrl+c again to confirm");
}

/* eine nachrichtenzeile. kein label mehr: der text startet direkt
 * am rand, links/rechts mit je einem leerzeichen padding.
 *
 * user-nachrichten bekommen einen leicht abgesetzten hintergrund
 * (THEME_ROLE_USER_BG) ueber die GANZE zeilenbreite – das rechte
 * padding schreibt row_finish mit dem hintergrund, abgeschaltet
 * wird er erst beim pad.
 *
 * ki-antworten (ASSISTANT) laufen durch den markdown-scanner: die
 * erste zeile einer nachricht kann ueberschrift (#), liste (-, *,
 * +, 1.) oder zitat (>) sein – marker/heading-zeichen werden in
 * der akzentfarbe gedruckt, ueberschriften zusaetzlich fett. der
 * scanner ist zeilenlokal (kein block-zustand): das highlight ist
 * damit schon waehrend des streamings stabil, wenn die zeile erst
 * halb da ist. */
/* markdown-bloecke der aktuellen live-nachricht: chat_wrap scannt
 * (fuer tabellen), row_msg fragt hier (fuer fence-highlight). der
 * zeiger gilt nur waehrend eines frames. */
static MdBlocks g_msg_blocks;
static bool g_msg_blocks_valid = false;

/* eine zeile in einem ```-fence: der inhalt wird token-fuer-token
 * gefaerbt. die fence-zeilen selbst (ticks) sind faint. tokens
 * ueber die GANZE nachricht konsistent: ein string, der in einer
 * umbruch-folgezeile weiterlaeuft, wird neu klassifiziert – der
 * tokenizer laeuft ab zeilenanfang, denn nur dort ist der
 * kontext (kommentar offen? string offen?) eindeutig genug fuer
 * ein streaming-sicheres bild. rudimentaer, aber stabil.
 *
 * text ist die ANZEIGE-KOPIE (chat_disp_text) – alle off/len der
 * ChatLines zeigen hinein. md_code_token bekommt sie inklusive
 * des terminators: strlen(text) endet an der kopie-grenze. */
static void row_code(Row *r, const ChatLine *ln, const char *text,
                     const char *lang)
{
    bool fence_line = (ln->lstart && ln->len >= 3 && text[ln->off] == '`' &&
                       text[ln->off + 1] == '`' && text[ln->off + 2] == '`');

    if (fence_line) {
        row_sgr(r, theme_role(THEME_ROLE_DIM));
        row_putn(r, text + ln->off, (int)ln->len);
        row_sgr(r, THEME_ROLE_RESET);
        return;
    }

    /* content: tokenizer ab zeilenanfang, ausgabe bis zeilenende.
     * tokens koennen uebers zeilenende hinausgehen (string ohne
     * schliesser, block-kommentar): sie werden an der zeile
     * abgeschnitten – der tokenizer läuft beim naechsten frame
     * wieder von vorn. */
    size_t lend = ln->off + ln->len;
    size_t i = ln->off;
    /* block-kommentare ueber mehrere zeilen werden ab ihrem
     * start-token gezaehlt; folgezeilen ohne eigenes start-token
     * sind normaler text (rudimentaer, dokumentiert). */
    while (i < lend) {
        MdTok tok;
        size_t n = md_code_token(text, i, lang, &tok);
        if (n == 0) {
            break;
        }
        size_t t_end = i + tok.len;
        if (t_end > lend) {
            t_end = lend; /* token ueber zeilenende: abschneiden */
        }
        switch (tok.kind) {
        case MD_TOK_KW:
            row_sgr(r, theme_role(THEME_ROLE_MD_KW));
            row_sgr(r, "\x1b[1m");
            break;
        case MD_TOK_STR:
            row_sgr(r, theme_role(THEME_ROLE_MD_STR));
            break;
        case MD_TOK_NUM:
            row_sgr(r, theme_role(THEME_ROLE_MD_NUM));
            break;
        case MD_TOK_COMMENT:
            row_sgr(r, theme_role(THEME_ROLE_MD_COMMENT));
            break;
        default:
            row_sgr(r, THEME_ROLE_RESET);
            break;
        }
        row_putn(r, text + i, (int)(t_end - i));
        i = t_end;
        if (t_end == i && tok.len == 0) {
            break;
        }
    }
    row_sgr(r, THEME_ROLE_RESET);
}

/* eine zeile [off,off+len) mit inline-markup drucken: **bold**
 * und `code`, auch verschachtelt (**`code`** = fett+hintergrund).
 *
 * die marker (ticks, stern-paare) werden NICHT gedruckt – die
 * anzeige zeigt nur den gerenderten inhalt, die rohdaten (chat,
 * session-log, api) bleiben unveraendert. die zeile wird dadurch
 * um die marker-bytes schmaler als die wrap-breite; chat_wrap
 * hat die marker bei der umbruchplanung mitgezaehlt, eine zeile
 * endet also evtl. ein paar zellen frueher. akzeptiert: die
 * alternative (marker mitdrucken) ist das kaputte bild, das
 * gemeldet wurde.
 *
 * stil je byte: die stile ALLER spans, die das byte enthalten,
 * kombiniert (bold + code = \x1b[1m + hintergrund). rueckstell-
 * ung paarweise: beim verlust eines stils nur DER stil off (22
 * fuer bold, 49 fuer hintergrund), damit laufende farben
 * ueberleben.
 *
 * streaming-sicher: unpaarige marker oeffnen kein span, ein
 * halber markup bleibt plain und stabilisiert sich mit dem
 * naechsten chunk. */
static void row_inline(Row *r, const char *text, size_t off, size_t len,
                       bool assistant)
{
    if (!assistant) {
        /* user-nachrichten: kein inline-markup (der benutzer tippt
         * literale backticks/sterne – die sollen nicht ploetzlich
         * als markup erscheinen) */
        row_putn(r, text + off, (int)len);
        return;
    }
    MdInline inl;
    md_inline_scan(text, off, len, &inl);

    bool in_bold = false; /* SGR 1 aktiv? */
    bool in_code = false; /* hintergrund aktiv? */
    size_t i = 0;
    while (i < len) {
        /* byte in bold-span? (irgendeine tiefe). spans beginnen
         * und enden an ascii-markern – ein mehrbyte-zeichen liegt
         * immer ganz in oder ganz ausserhalb eines spans, der
         * erste byte des zeichentscheidet */
        bool b = false;
        bool c = false;
        for (int k = 0; k < inl.n; k++) {
            if (i >= inl.start[k] && i < inl.end[k]) {
                if (inl.kinds[k] == MD_INL_BOLD) {
                    b = true;
                } else {
                    c = true;
                }
            }
        }
        if (md_inline_marker(&inl, i)) {
            /* marker-bytes verschwinden aus der anzeige – sie
             * zaehlen nicht als zelle, kein putc */
            i++;
            continue;
        }
        if (b != in_bold) {
            row_sgr(r, b ? theme_role(THEME_ROLE_MD_BOLD) : "\x1b[22m");
            in_bold = b;
        }
        if (c != in_code) {
            row_sgr(r, c ? theme_role(THEME_ROLE_MD_CODE) : THEME_ROLE_BG_OFF);
            in_code = c;
        }
        /* GANZES utf-8-zeichen ausgeben: die zelle zaehlt einmal,
         * nicht je byte – sonst waeren umlaute wieder 2 zellen
         * breit und der wrap-plan des chat_wrap wuerde vom renderer
         * abweichen (dasselbe problem wie vorher) */
        size_t clen = row_utf8_step(text + off + i);
        if (i + clen > len) {
            clen = 1; /* kann kaum passieren: chat_wrap teilt nie */
        }
        row_putn(r, text + off + i, (int)clen);
        i += clen;
    }
    if (in_bold) {
        row_sgr(r, "\x1b[22m");
    }
    if (in_code) {
        row_sgr(r, THEME_ROLE_BG_OFF);
    }
}

/* eine zeile [off,off+len) mit inline-markup drucken (**bold**,
 * `code`). die marker werden DIM mitgedruckt (chat_wrap hat sie
 * bei der breitenplanung mitgezaehlt – weg lassen wuerde die zeile
 * gegenueber dem wrap verschieben), der inhalt bekommt seinen stil.
 * spans ohne schliesser auf der zeile sind plain (streaming:
 * stabilisiert sich mit dem naechsten chunk).
 *
 * die stil-rueckstellung ist absichtlich kleinteilig: BOLD wird
 * mit SGR 22 (nur bold off) zurueckgestellt, die farbe bleibt –
 * sonst wuerde bold in einer farbigen zeile die farbe fressen.
 * CODE (hintergrund) wird mit BG_OFF (49) beendet. */

static void row_msg(Row *r, const ChatLine *ln, const char *text)
{
    /* text = die anzeige-kopie (chat_disp_text): die dekodierten
     * texte, in die die off/len von ln zeigen */
    bool user = (ln->role == CHAT_ROLE_USER);

    if (user) {
        row_sgr(r, theme_role(THEME_ROLE_USER_BG));
        row_sgr(r, theme_role(THEME_ROLE_USER));
    }

    row_putc(r, ' '); /* linkes padding */

    /* code-fence: block-tabelle fragen (gilt nur fuer ki-antworten
     * und nur, wenn der frame die blocks gesetzt hat) */
    if (ln->role == CHAT_ROLE_ASSISTANT && g_msg_blocks_valid) {
        const MdBlock *blk = md_block_at(&g_msg_blocks, ln->off);
        if (blk != NULL && blk->kind == MD_BLK_CODE) {
            row_code(r, ln, text, blk->lang);
            row_putc(r, ' '); /* rechtes padding */
            row_sgr(r, THEME_ROLE_RESET);
            return;
        }
    }

    if (ln->role == CHAT_ROLE_ASSISTANT && ln->lstart) {
        /* markdown-scanning nur am ORIGINALEN zeilenanfang (lstart,
         * nicht first: jede nach einem \n beginnende zeile ist ein
         * kandidat, nur wrap-folgezeilen nicht) */
        MdLine md;
        md_scan(text, ln->off, ln->len, ln->lstart, &md);
        switch (md.kind) {
        case MD_HEAD: {
            /* die #-folge farbig+fett, dahinter normal weiter */
            row_sgr(r, theme_role(THEME_ROLE_MD_HEAD));
            row_sgr(r, theme_current()->match);
            row_putn(r, text + md.marker, (int)md.marker_len);
            row_sgr(r, THEME_ROLE_RESET);
            row_inline(r, text, ln->off + md.marker_len,
                       ln->len - md.marker_len, true);
            break;
        }
        case MD_LIST:
        case MD_QUOTE: {
            /* der marker farbig, dahinter der text normal */
            row_sgr(r, theme_role(THEME_ROLE_MD_MARKER));
            row_putn(r, text + md.marker, (int)md.marker_len);
            row_sgr(r, THEME_ROLE_RESET);
            row_inline(r, text, ln->off + md.marker_len,
                       ln->len - md.marker_len, true);
            break;
        }
        case MD_NONE:
        default:
            row_inline(r, text, ln->off, ln->len, true);
            break;
        }
    } else {
        row_inline(r, text, ln->off, ln->len, ln->role == CHAT_ROLE_ASSISTANT);
    }

    /* rechtes padding: bei user-nachrichten gehoert es mit zum
     * hintergrund, row_finish raumt den rest mit BG_OFF */
    row_putc(r, ' ');
    if (user) {
        row_sgr(r, THEME_ROLE_BG_OFF);
        row_sgr(r, THEME_ROLE_RESET);
    } else {
        row_sgr(r, THEME_ROLE_RESET);
    }
}

/* darstellungs-zeilen eines tool-calls. uebergrosse aufrufe (z.B.
 * bash mit sehr langen parametern) hat chat_wrap am zeilenrand
 * umgebrochen: off/len verweisen auf den darstellungs-string, die
 * fortsetzungs-zeilen (off > 0) ruecken um TOOL_INDENT_W ein. die
 * umbruch-breite folgt dem terminal – neu gedruckte calls rechnen
 * mit der aktuellen spaltenzahl, ein resize wirkt sofort. */
static void row_tool_call(Row *r, const ChatToolCall *call, const ChatLine *ln)
{
    row_sgr(r, theme_role(THEME_ROLE_TOOL));
    /* das label/indent liefert der aufrufer: bei "ai"-gelabelten
     * calls steht der pfeil direkt dahinter */
    if (ln->off > 0) {
        for (int i = 0; i < TOOL_INDENT_W; i++) {
            row_putc(r, ' ');
        }
    }
    char *s = chat_tool_display(call);
    if (s == NULL) {
        die("out of memory");
    }
    row_putn(r, s + ln->off, (int)ln->len);
    free(s);
    row_sgr(r, THEME_ROLE_RESET);
}

/* millisekunden als kurze dauer: "42s", "1m 07s", "2h 03m" */
static void fmt_dur(long long ms, char *buf, size_t sz)
{
    long long s = ms / 1000;
    if (s < 60) {
        (void)snprintf(buf, sz, "%llds", s);
    } else if (s < 3600) {
        (void)snprintf(buf, sz, "%lldm %02llds", s / 60, s % 60);
    } else {
        (void)snprintf(buf, sz, "%lldh %02lldm", s / 3600, (s / 60) % 60);
    }
}

/* braille-lade-animation (10 frames): die rahmen-zeile des docks
 * waehrend des turns und der live-spinner im chat laufen damit */
static const char *const SPIN[] = {
    "\xE2\xA0\x8B", "\xE2\xA0\x99", "\xE2\xA0\xB9", "\xE2\xA0\xB8",
    "\xE2\xA0\xBC", "\xE2\xA0\xB4", "\xE2\xA0\xA6", "\xE2\xA0\xA7",
    "\xE2\xA0\x87", "\xE2\xA0\x88",
};

static int spin_frame(long long busy_ms)
{
    return (int)((busy_ms / 100) % 10);
}

/* live-spinner im chat, waehrend die ki an einem tool arbeitet */
static void row_tool_spinner(Row *r, long long busy_ms)
{
    row_puts(r, " "); /* padding wie alle nachrichtenzeilen */
    row_sgr(r, theme_role(THEME_ROLE_DIM));
    row_glyph(r, SPIN[spin_frame(busy_ms)]);
    row_sgr(r, THEME_ROLE_RESET);
}

/* "exit: N": der bash-anhang. rueckgabe: N, oder -1 wenn dieser
 * zeilenabschnitt KEIN exit-marker ist. die faerbung geschieht im
 * renderer (gruen/rot ueber die theme-rollen MD_OK/MD_ERR); das
 * model und das session-log sehen den text weiterhin unfaerbt. */
static int exit_marker_of(const char *text, const ChatLine *ln)
{
    const char *s = text + ln->off; /* text = anzeige-kopie */
    size_t len = ln->len;
    /* "exit: N": 6 zeichen praefix + 1..8 ziffern */
    if (len < 7 || len > 14) {
        return -1;
    }
    if (strncmp(s, "exit: ", 6) != 0) {
        return -1;
    }
    int code = 0;
    for (size_t i = 6; i < len; i++) {
        if (s[i] < '0' || s[i] > '9') {
            return -1;
        }
        code = (code * 10) + (s[i] - '0');
    }
    return code;
}

/* obere rahmenzeile des eingabefelds, solange die ki arbeitet:
 * braille-spinner + laufende sekunden, dahinter der rest der
 * trennlinie. die animation tritt im watchdog-takt (~100ms, siehe
 * stream_should_abort) und laeuft auch, wenn das modell nur denkt
 * und kein chunk fliesst. */
static void row_busy_border(Row *r, long long busy_ms)
{
    int frame = spin_frame(busy_ms);
    char dur[24];
    fmt_dur(busy_ms, dur, sizeof dur);

    row_sgr(r, theme_role(THEME_ROLE_DIM));
    row_glyph(r, GLYPH_EM_DASH);
    row_glyph(r, GLYPH_EM_DASH);
    row_glyph(r, " ");
    row_glyph(r, SPIN[frame]);
    row_sgr(r, THEME_ROLE_RESET);
    row_sgr(r, theme_role(THEME_ROLE_DIM));
    row_glyph(r, " ");
    row_puts(r, dur);
    row_glyph(r, " ");
    row_sgr(r, THEME_ROLE_RESET);
    row_border(r); /* rest der linie */
}

/* ------------------------------------------------------------------ */
/* zeilen-ausgabe: \r + zeile loeschen, inhalt, optional umbruch. der  */
/* umbruch scrollt am unteren rand ganz von selbst – genau so        */
/* waechst das terminal-scrollback                                     */
/* ------------------------------------------------------------------ */

static void row_start(int main_w)
{
    /* 4 byte je zelle (utf-8-maximum) PLUS 512 byte ansi-spielraum:
     * seit der codepoint-zaehlung koennen main_w zellen je bis zu
     * 4 byte text tragen – die sgr-sequenzen (farben, bold,
     * zuruecksetzungen, je bis ~10 byte) muessen DANACH noch
     * passen, sonst schreibt row_sgr ueber das puffer-ende */
    size_t need = ((size_t)main_w * 4U) + 512U;
    if (g_row_buf_cap < need) {
        char *grown = realloc(g_row_buf, need);
        if (grown == NULL) {
            die("out of memory");
        }
        g_row_buf = grown;
        g_row_buf_cap = need;
    }
    g_frame.buf = g_row_buf;
    g_frame.pos = 0;
    g_row.f = &g_frame;
    g_row.width = main_w;
    g_row.cells = 0;
    flush_puts("\r\x1b[K");
}

static void row_finish(bool newline)
{
    row_pad(&g_row);
    flush_put(g_frame.buf, g_frame.pos);
    if (newline) {
        flush_put("\n", 1);
    }
}

/* ------------------------------------------------------------------ */
/* render-zustand: was schon im scrollback landete                    */
/* ------------------------------------------------------------------ */

static size_t g_printed = 0;  /* chat.msgs[0..g_printed) sind gedruckt */
static size_t g_live_msg = 0; /* index der live-nachricht (nur mit
                               * g_live_msg_valid) */
static bool g_live_msg_valid = false;
static size_t g_live_lines = 0;    /* davon bereits committete zeilen */
static int g_prev_rows = 0;        /* live+dock-zeilen des letzten frames */
static int g_busy_up = 0;          /* spinner-rahmen: zeilen ueber dem cursor */
static int g_tool_spin_up = 0;     /* tool-spinner im chat: ueber dem cursor */
static bool g_tail_only = false;   /* content ersetzt: schwanz drucken */
static bool g_printed_any = false; /* es wurde schon content gedruckt:
                                    * die erste nachricht einer sitzung
                                    * beginnt ohne abstand nach oben */

/* wrap-arena: nur der transkript-schwanz wird umgebrochen (nicht
 * mehr jedes frame ALLES wie im vollbild) */
static ChatLine *g_lines = NULL;
static size_t g_lines_cap = 0;
static size_t g_nlines = 0;

void draw_set_out(FILE *o)
{
    g_out = o;
}

void draw_content_reset(void)
{
    g_printed = 0;
    g_live_msg = 0;
    g_live_msg_valid = false;
    g_live_lines = 0;
    g_printed_any = false;
    /* der chat wurde ersetzt: beim naechsten frame nur den schwanz
     * drucken, der auf einen bildschirm passt – aelteres bleibt im
     * scrollback, wo es eh schon zu lesen war */
    g_tail_only = true;
}

void draw_reset(int rows, bool full_reprint)
{
    dbg("draw: reset (%d zeilen scrollen, full=%d)", rows, (int)full_reprint);
    /* nach einem resize hat das terminal umgebrochen – der
     * relative cursor-zustand ist unbrauchbar. bis zum boden
     * scrollen (der cursor sitzt danach garantiert unten) und den
     * dock frisch aufsetzen. */
    for (int i = 0; i < rows; i++) {
        flush_put("\r\n", 2);
    }
    flush_out();
    g_prev_rows = 0;
    g_busy_up = 0; /* layout ungueltig: nur volle frames */
    g_tool_spin_up = 0;
    /* RESIZE: der alte content steht im scrollback falsch umge-
     * brochen – unlesbar, aber noch da. der ganze chat wird im
     * naechsten frame MIT DER NEUEN BREITE erneut gedruckt:
     * g_printed faellt auf 0, der renderer setzt alle nachrich-
     * ten neu an. darueber bleibt der zerbrochene alte rest im
     * scrollback, der neue ist lesbar.
     *
     * g_tail_only (nur der schwanz nach /new und resume) gehoert
     * hier NICHT her: dort wurde der inhalt ERSETZT, hier ist er
     * derselbe – nur die geometrie hat sich geaendert. ein
     * budget-kappung beim resize haette die folgen-dran-nach-
     * richten im verlauf verloren (bis zum naechsten resize
     * fehlten alle aelteren im scrollback). */
    if (full_reprint) {
        g_printed = 0;
        g_live_msg = 0;
        g_live_msg_valid = false;
        g_live_lines = 0;
    }
}

/* ------------------------------------------------------------------ */
/* dock: die zeilen, die unten angedockt jede frame neu gezeichnet   */
/* werden (eingabe inkl. rahmen, befehlsliste, dialoge, status)      */
/* ------------------------------------------------------------------ */

typedef enum {
    DROW_GAP,           /* abstand ueber dem dock; zeigt solange die
                         * quit-bestaetigung an (DROW_QUUIT) */
    DROW_BORDER,        /* trenn-linie aus em-dashes */
    DROW_BUSY_BORDER,   /* rahmen mit spinner + sekunden (ki arbeitet) */
    DROW_INPUT,         /* a = sichtbare eingabezeile */
    DROW_CMD,           /* a = befehl-index (COMMANDS) */
    DROW_CMD_EMPTY,     /* hinweis: kein befehl passt */
    DROW_QUIT,          /* quit-bestaetigung */
    DROW_STATUS_MODEL,  /* statuszeile 1 */
    DROW_STATUS_TOKENS, /* statuszeile 2 */
    DROW_DLG_BORDER,    /* trenn-linie ueber dem dialog */
    DROW_DLG_SEARCH,    /* such-zeile mit cursor */
    DROW_DLG_EMPTY,     /* hinweis: kein eintrag passt */
    DROW_DLG_ENTRY,     /* a = flacher listen-index */
} DockRowKind;

typedef struct {
    DockRowKind kind;
    int a;    /* kind-abhaengiger index */
    bool sel; /* eintrag angewaehlt */
} DockRow;

typedef struct {
    int rows;
    int main_w;
    UIMode mode;

    /* eingabe */
    int input_w;
    size_t input_first;

    /* befehlsliste (MODE_INPUT) */
    char prefix[64];
    int cmd_idx[COMMAND_COUNT];
    int cmd_n;

    /* dialoge */
    char search[64];
    int match_idx[DIALOG_MATCH_MAX];
    int match_count;
    int id_col;
    int name_col;
    int sel_idx;            /* flacher index des angewaehlten eintrags */
    long long busy_ms;      /* laufende arbeitszeit des turns (spinner) */
    int busy_row_up;        /* spinner-rahmen: zeilen ueber dem geparkten
                             * cursor (anker fuer den leichten tick) */
    int status_model_lines; /* belegte zeilen der model-statuszeile */
    int status_token_lines; /* belegte zeilen der token-statuszeile */
} DockCtx;

static const char *dlg_title(UIMode mode)
{
    switch (mode) {
    case MODE_MODELS:
        return "models";
    case MODE_SETTINGS:
        return "settings";
    case MODE_THEME:
        return "theme";
    case MODE_SESSIONS:
        return "resume";
    default:
        return "";
    }
}

int input_field_width(int cols)
{
    /* INPUT_PREFIX_W ist 0: der text nutzt die volle breite minus
     * die spalte, in der der cursor-block am zeilenende sitzt */
    int w = main_width(cols) - INPUT_PREFIX_W - 1;
    return (w > 0) ? w : 1;
}

/* cursor/scroll normalisieren und in den state zurueckschreiben,
 * damit die key-behandlung (enter) konsistente werte sieht */
static void dialog_normalize(DockCtx *d, DialogState *st_dialog, int entries_h)
{
    if (st_dialog->selected >= d->match_count) {
        st_dialog->selected = d->match_count - 1;
    }
    if (st_dialog->selected < 0) {
        st_dialog->selected = 0;
    }
    if (st_dialog->scroll + entries_h > d->match_count) {
        st_dialog->scroll = d->match_count - entries_h;
    }
    if (st_dialog->scroll < 0) {
        st_dialog->scroll = 0;
    }
    if (st_dialog->selected < st_dialog->scroll) {
        st_dialog->scroll = st_dialog->selected;
    }
    if (st_dialog->selected >= st_dialog->scroll + entries_h) {
        st_dialog->scroll = st_dialog->selected - entries_h + 1;
    }
}

/* dialog-treffer je modus berechnen und spaltenbreiten setzen */
static void dialog_matches(int rows, AppState *st, const Config *cfg,
                           DockCtx *d)
{
    int matches[DIALOG_MATCH_MAX];

    switch (d->mode) {
    case MODE_MODELS:
        snprintf(d->search, sizeof d->search, "%s", st->dialog.search);
        d->match_count =
            models_match(cfg, st->dialog.search, matches, DIALOG_MATCH_MAX);
        for (int i = 0; i < d->match_count; i++) {
            d->match_idx[i] = matches[i];
            const char *url = "";
            const Model *m = model_at(cfg, matches[i], &url);
            if (m != NULL && m->id != NULL) {
                int w = (int)strlen(m->id);
                if (w > d->id_col) {
                    d->id_col = w;
                }
            }
        }
        break;
    case MODE_SETTINGS:
        snprintf(d->search, sizeof d->search, "%s", st->dialog.search);
        d->match_count =
            names_match(SETTING_NAMES, SET_COUNT, st->dialog.search, matches,
                        DIALOG_MATCH_MAX);
        for (int i = 0; i < d->match_count; i++) {
            d->match_idx[i] = matches[i];
        }
        d->id_col = names_col(SETTING_NAMES, SET_COUNT);
        break;
    case MODE_THEME: {
        const char *names[16] = {NULL};
        int total = theme_names(names, (int)(sizeof names / sizeof names[0]));
        snprintf(d->search, sizeof d->search, "%s", st->dialog.search);
        d->match_count = names_match(names, total, st->dialog.search, matches,
                                     DIALOG_MATCH_MAX);
        for (int i = 0; i < d->match_count; i++) {
            d->match_idx[i] = matches[i];
        }
        d->id_col = names_col(names, total);
        break;
    }
    case MODE_SESSIONS: {
        snprintf(d->search, sizeof d->search, "%s", st->dialog.search);
        d->match_count = sessions_match(&st->sessions, st->dialog.search,
                                        matches, DIALOG_MATCH_MAX);
        for (int i = 0; i < d->match_count; i++) {
            d->match_idx[i] = matches[i];
            int idw = (int)strlen(st->sessions.items[(size_t)matches[i]].id);
            if (idw > d->id_col) {
                d->id_col = idw;
            }
        }
        /* name-spalte: nur, was nach id, datum und nachrichten-zahl
         * uebrig bleibt (3 " > " + 2 + id + 2 + 11 + 5 + 16) */
        int free_w = d->main_w - d->id_col - 39;
        if (free_w < 1) {
            free_w = 1;
        }
        for (int i = 0; i < d->match_count; i++) {
            const SessionInfo *si = &st->sessions.items[(size_t)matches[i]];
            int nw = 0;
            if (si->name != NULL) {
                nw = (int)strlen(si->name);
            } else if (si->preview != NULL) {
                nw = (int)strlen(si->preview);
            }
            if (nw > 48) {
                nw = 48;
            }
            if (nw > free_w) {
                nw = free_w;
            }
            if (nw > d->name_col) {
                d->name_col = nw;
            }
        }
        break;
    }
    default:
        break;
    }
    (void)rows;
}

/* dock-zeilen von oben nach unten aufbauen. rueckgabe: anzahl */
static int dock_build(int rows, int cols, AppState *st, const Config *cfg,
                      DockCtx *d, DockRow *out_rows, int out_max)
{
    memset(d, 0, sizeof *d);
    d->rows = rows;
    d->main_w = main_width(cols);
    d->mode = ui_mode(st);

    int n = 0;
    /* statuszeilen: bei schmalem terminal bricht der inhalt um.
     * die zeilenzahl steht hier fest (1..STATUS_MAX_LINES je
     * zeile), damit usable die eingabebox korrekt eingrenzt und
     * die rows unten die richtigen teil-zeilen anfordern */
    {
        StatusLine m = {0};
        StatusLine t = {0};
        status_model_segs(&m, cfg);
        status_token_segs(&t, st);
        int w = d->main_w - STATUS_LABEL_W;
        if (w < 1) {
            w = 1;
        }
        d->status_model_lines = st_lines(&m, w);
        d->status_token_lines = st_lines(&t, w);
    }
    int usable = rows - d->status_model_lines - d->status_token_lines;
    if (usable < 1) {
        usable = 1;
    }

    /* abstands-zeile zwischen verlauf und eingabefeld: sie ist
     * teil des docks und bleibt damit stehen, egal wieviel neuer
     * verlauf druckt. ein mal ctrl+c gedrueckt, zeigt genau diese
     * zeile die quit-warnung, der zweite druck konfirmiert */
    if (n < out_max) {
        if (st->confirm_quit) {
            out_rows[n++] = (DockRow){DROW_QUIT, 0, false};
        } else {
            out_rows[n++] = (DockRow){DROW_GAP, 0, false};
        }
    }

    if (d->mode == MODE_INPUT) {
        /* eingabe-geometrie: die box waechst mit dem text, der dock
         * laesst oben platz fuer live-zeile und verlauf */
        int list_h = cmd_list_height(st);
        /* die quit-bestaetigung belegt die abstands-zeile, der dock
         * ist in beiden zustaenden gleich hoch */
        int input_bottom = bottom_border_for(usable, list_h, false);
        d->input_w = input_field_width(cols);
        size_t in_rows = input_screen_rows(&st->input, d->input_w);
        int max_h = input_bottom - 4; /* eine zeile mehr fuer den abstand */
        if (max_h < 1) {
            max_h = 1;
        }
        int in_h = (in_rows > (size_t)max_h) ? max_h : (int)in_rows;
        if (in_h < 1) {
            in_h = 1;
        }
        d->input_first = 0;
        if (in_rows > (size_t)in_h) {
            size_t crow = 0;
            input_cursor_screen(&st->input, d->input_w, &crow, NULL);
            if (crow >= (size_t)in_h) {
                d->input_first = crow - (size_t)in_h + 1;
            }
            if (d->input_first + (size_t)in_h > in_rows) {
                d->input_first = in_rows - (size_t)in_h;
            }
        }

        /* solange die ki arbeitet, ist die OBERE rahmenzeile des
         * eingabefelds die lade-animation (spinner + sekunden) */
        d->busy_ms = (st->busy && st->busy_start_ms > 0)
                         ? mono_ms() - st->busy_start_ms
                         : 0;

        if (n + in_h + 2 <= out_max) {
            if (st->busy) {
                d->busy_row_up = n; /* index, am ende in 'up' umrechnen */
                out_rows[n++] = (DockRow){DROW_BUSY_BORDER, 0, false};
            } else {
                out_rows[n++] = (DockRow){DROW_BORDER, 0, false};
            }
            for (int i = 0; i < in_h; i++) {
                out_rows[n++] = (DockRow){DROW_INPUT, i, false};
            }
            out_rows[n++] = (DockRow){DROW_BORDER, 0, false};
        }

        /* befehlsliste direkt unter dem eingabefeld */
        if (st->cmd_active) {
            cmd_prefix(&st->input, d->prefix, sizeof d->prefix);
            d->cmd_n = cmd_match(d->prefix, d->cmd_idx, COMMAND_COUNT);
            if (d->cmd_n > 0) {
                for (int i = 0; i < d->cmd_n && n < out_max; i++) {
                    out_rows[n++] = (DockRow){DROW_CMD, d->cmd_idx[i], false};
                }
            } else if (n < out_max) {
                out_rows[n++] = (DockRow){DROW_CMD_EMPTY, 0, false};
            }
        }
    } else {
        /* dialog statt eingabefeld: border, suchzeile, eintraege */
        dialog_matches(rows, st, cfg, d);
        int entries_h = (d->match_count > 0) ? d->match_count : 1;
        if (entries_h > usable / 2) {
            entries_h = usable / 2;
        }
        if (entries_h < 1) {
            entries_h = 1;
        }
        dialog_normalize(d, &st->dialog, entries_h);
        d->sel_idx =
            (d->match_count > 0) ? d->match_idx[st->dialog.selected] : -1;

        if (n + entries_h + 2 <= out_max) {
            out_rows[n++] = (DockRow){DROW_DLG_BORDER, 0, false};
            out_rows[n++] = (DockRow){DROW_DLG_SEARCH, 0, false};
            if (d->match_count == 0) {
                out_rows[n++] = (DockRow){DROW_DLG_EMPTY, 0, false};
            } else {
                for (int i = 0; i < entries_h; i++) {
                    int vis = st->dialog.scroll + i;
                    if (vis >= d->match_count) {
                        break;
                    }
                    int idx = d->match_idx[vis];
                    out_rows[n++] =
                        (DockRow){DROW_DLG_ENTRY, idx, idx == d->sel_idx};
                }
            }
        }
    }

    for (int sub = 0; sub < d->status_model_lines && n < out_max; sub++) {
        out_rows[n++] = (DockRow){DROW_STATUS_MODEL, sub, false};
    }
    for (int sub = 0; sub < d->status_token_lines && n < out_max; sub++) {
        out_rows[n++] = (DockRow){DROW_STATUS_TOKENS, sub, false};
    }
    /* spinner-rahmen in "zeilen ueber dem geparkten cursor" um-
     * rechnen (anker fuer draw_busy_tick) */
    if (d->busy_row_up > 0) {
        d->busy_row_up = n - 1 - d->busy_row_up;
    }
    return n;
}

/* ------------------------------------------------------------------ */
/* frame                                                              */
/* ------------------------------------------------------------------ */

static void lines_reserve(size_t need)
{
    if (g_lines_cap >= need) {
        return;
    }
    size_t cap = (g_lines_cap > 0) ? g_lines_cap : 64;
    while (cap < need) {
        if (cap > SIZE_MAX / 2) {
            die("out of memory");
        }
        cap *= 2;
    }
    ChatLine *grown = realloc(g_lines, cap * sizeof *grown);
    if (grown == NULL) {
        die("out of memory");
    }
    g_lines = grown;
    g_lines_cap = cap;
}

/* chat [from..len) wrappen; ergebnis in g_lines/g_nlines. die
 * ChatLine.msg-indizes sind RELATIV zu `from` */
static void wrap_tail(const Chat *chat, size_t from, int text_w)
{
    g_nlines = 0;
    g_msg_blocks_valid = false;
    if (from >= chat->len) {
        return;
    }
    Chat sub = {0};
    sub.msgs = chat->msgs + from;
    sub.len = chat->len - from;
    size_t need = chat_wrap(&sub, text_w, NULL, 0);
    lines_reserve(need);
    (void)chat_wrap(&sub, text_w, g_lines, g_lines_cap);
    g_nlines = need;
    /* block-tabelle fuer den fence-highlight: die letzte
     * ki-nachricht im schwanz (ihre zeilen sind die, die row_msg
     * gleich zeichnet). committete nachrichten brauchen keine
     * mehr – sie sind im scrollback, ein neuer frame zeichnet
     * nur die frontier und die live-zeile. */
    for (size_t m = chat->len; m > from; m--) {
        const ChatMessage *msg = &chat->msgs[m - 1];
        if (msg->role == CHAT_ROLE_ASSISTANT && msg->text != NULL) {
            /* der block-scan laeuft ueber die ANZEIGE-KOPIE: die
             * ChatLines zeigen hinein, die block-grenzen muessen im
             * selben koordinatensystem liegen */
            const char *disp = chat_disp_text();
            size_t doff = chat_disp_off(m - 1 - from);
            md_block_scan(disp + doff, &g_msg_blocks);
            for (int b = 0; b < g_msg_blocks.n; b++) {
                g_msg_blocks.blocks[b].start += doff;
                g_msg_blocks.blocks[b].end += doff;
            }
            g_msg_blocks_valid = true;
            break;
        }
    }
}

/* zeilenbereich [s,e) der relativen nachricht m in g_lines */
static void msg_line_range(size_t m, size_t *s, size_t *e)
{
    *s = 0;
    *e = 0;
    for (size_t i = 0; i < g_nlines; i++) {
        if (g_lines[i].msg < m) {
            continue;
        }
        if (g_lines[i].msg > m) {
            break;
        }
        if (*e == 0) {
            *s = i;
        }
        *e = i + 1;
    }
}

/* eine dock-zeile rendern */
static void render_dock_row(const DockRow *row, const DockCtx *d, AppState *st,
                            const Config *cfg)
{
    Row *r = &g_row;
    switch (row->kind) {
    case DROW_GAP:
        /* leer – die zeile raeumt row_start selbst */
        break;
    case DROW_BUSY_BORDER:
        row_busy_border(r, d->busy_ms);
        break;
    case DROW_BORDER:
    case DROW_DLG_BORDER:
        row_border(r);
        break;
    case DROW_INPUT:
        row_input(r, &st->input, d->input_w, d->input_first + (size_t)row->a);
        break;
    case DROW_CMD:
        row_command(r, &COMMANDS[row->a], d->prefix);
        break;
    case DROW_CMD_EMPTY:
        row_no_match(r, d->prefix);
        break;
    case DROW_QUIT:
        row_quit(r);
        break;
    case DROW_STATUS_MODEL:
        row_status_model(r, cfg, row->a);
        break;
    case DROW_STATUS_TOKENS:
        row_status_tokens(r, st, row->a);
        break;
    case DROW_DLG_SEARCH:
        row_dlg_search(r, dlg_title(d->mode), d->search);
        break;
    case DROW_DLG_EMPTY:
        row_no_dlg_match(r, d->search);
        break;
    case DROW_DLG_ENTRY:
        switch (d->mode) {
        case MODE_MODELS:
            row_model(r, cfg, row->a, d->search, row->sel, d->id_col);
            break;
        case MODE_SETTINGS:
            row_setting(r, cfg, row->a, d->search, row->sel, d->id_col);
            break;
        case MODE_THEME: {
            const char *name = theme_option_name(row->a);
            bool active = false;
            if (name != NULL && strcmp(name, theme_current()->name) == 0) {
                active = true;
            }
            row_theme_opt(r, row->a, d->search, row->sel, active, d->id_col);
            break;
        }
        case MODE_SESSIONS: {
            const SessionInfo *si = &st->sessions.items[(size_t)row->a];
            bool active = false;
            if (st->session.active && strcmp(st->session.id, si->id) == 0) {
                active = true;
            }
            row_session(r, si, d->search, row->sel, active, d->id_col,
                        d->name_col);
            break;
        }
        default:
            break;
        }
        break;
    }
}

void draw(int rows, int cols, AppState *state, const Config *cfg)
{
    Chat *chat = &state->chat;
    int main_w = main_width(cols);
    int text_w = main_w - (MSG_PAD_W * 2);
    if (text_w < 1) {
        text_w = 1;
    }

    /* ---- dock-geometrie (vor content: dialog-cursor wird hier
     *      normalisiert, darauf verlassen die key-handler) ---- */
    DockCtx d;
    DockRow dock[DOCK_ROWS_MAX];
    int dock_n = dock_build(rows, cols, state, cfg, &d, dock, DOCK_ROWS_MAX);

    /* ---- content-reset (nach /new oder resume): nur der schwanz ---- */
    size_t from = g_printed;
    if (g_tail_only) {
        g_tail_only = false;
        g_printed = 0;
        g_live_msg = 0;
        g_live_msg_valid = false;
        g_live_lines = 0;
        from = 0;
        if (chat->len > 0) {
            wrap_tail(chat, 0, text_w);
            /* budget: die zeilen, die neben dock und live-zeile auf
             * den bildschirm passen */
            int budget = rows - dock_n - 2;
            if (budget < 0) {
                budget = 0;
            }
            size_t acc = 0;
            for (size_t mi = chat->len; mi > 0; mi--) {
                size_t s = 0;
                size_t e = 0;
                msg_line_range(mi - 1, &s, &e);
                acc += e - s;
                if ((int)acc > budget && mi > 1) {
                    from = mi - 1;
                    break;
                }
            }
            g_printed = from;
        }
    }
    if (from > chat->len) {
        from = chat->len; /* chat geleert: nichts nachzudrucken */
    }

    /* ---- live-nachricht: die letzte assistant-nachricht, solange
     *      die anfrage laeuft (streaming-platzhalter) ---- */
    size_t live_idx = SIZE_MAX;
    bool live_open_block = false; /* live-nachricht enthaelt einen
                                   * block, der noch waechst */
    size_t live_keep = 0;         /* offener block: so viele zeilen von
                                   * unten bleiben live (budget), der rest
                                   * committet in den scrollback */
    if (state->busy && chat->len > 0 &&
        chat->msgs[chat->len - 1].role == CHAT_ROLE_ASSISTANT &&
        chat->msgs[chat->len - 1].tool_calls_len == 0) {
        live_idx = chat->len - 1;
        /* offener block: der letzte block reicht bis ans text-ende
         * (end == tlen) – der scanner schliesst dort immer, ob
         * mit echtem schliesser oder streaming-offen. beim
         * committen wuerden halbfertige tabellen-breiten bzw. der
         * wachsende fence eingefroren; die nachricht bleibt
         * deshalb (budgetiert) live. */
        /* block-scan JETZT (nicht erst in wrap_tail): der
         * open-detect braucht die tabelle zum text DIESES
         * frames. mit dem vorframes-stand feuerte er einen
         * frame zu spaet und committete in den oeffnenden
         * block hinein (doppelte zeilen im scrollback). */
        /* KEIN block-scan hier mehr: die anzeige-kopie des
         * VORframes hat stale offsets (nach einem resize passt
         * chat_disp_off nicht mehr zur neuen tail-grenze), und
         * wrap_tail fuellt g_msg_blocks ohnehin frisch fuer den
         * frame. der offene-block-entscheid (live_open_block)
         * trifft der scan NACH wrap_tail weiter unten – autorita-
         * tiv und mit den grenzen DIESES frames. */
    }

    /* platzhalter wurde entfernt (fehler/abbruch): frontier
     * nachfuehren – gepoppt wird nur die leere nachricht, es kann
     * keine committeten zeilen geben */
    if (g_live_msg_valid && g_live_msg >= chat->len) {
        g_live_msg_valid = false;
        g_live_lines = 0;
        if (g_printed > chat->len) {
            g_printed = chat->len;
        }
    }

    /* ---- wrap des schwanzes ---- */
    wrap_tail(chat, from, text_w);

    /* die anzeige-kopie des schwanzes: alle texte dekodiert (\n
     * echte newlines, \t spacen, \uXXXX zeichen), die off/len der
     * g_lines zeigen hinein. ab hier liest der frame nachricht-
     * texte NIE mehr direkt aus ChatMessage. */
    const char *disp = chat_disp_text();

    /* offener block: nochmal mit den FRISCHEN block-grenzen pruefen
     * (wrap_tail hat die tabelle gerade auf die kopie umgerechnet).
     * live_open_block wird hier endgueltig festgelegt, BEVOR das
     * committen anfaengt; die erkennung weiter oben ist nur die
     * fruehe vorstufe fuer die g_msg_blocks-gueltigkeit. */
    if (live_idx != SIZE_MAX && from <= live_idx) {
        const char *lt = disp + chat_disp_off(live_idx - from);
        if (lt[0] != '\0') {
            MdBlocks fresh;
            md_block_scan(lt, &fresh);
            bool open = false;
            for (int b = fresh.n - 1; b >= 0; b--) {
                const MdBlock *blk = &fresh.blocks[b];
                /* offen = der letzte block reicht bis ans kopie-ende */
                if (blk->end == strlen(lt) && blk->end > blk->start &&
                    (blk->kind == MD_BLK_CODE || blk->kind == MD_BLK_TABLE)) {
                    open = true;
                }
                break; /* nur der letzte block zaehlt */
            }
            live_open_block = open;
        }
    }

    /* offener block: budget fuer die live-region. msg_line_range
     * braucht die FRISCHEN g_lines – deshalb erst nach dem wrap.
     * die letzten live_keep zeilen der live-nachricht bleiben
     * ueberschreibbar (mit dem dock zusammen auf dem bildschirm),
     * der anfang committet in den scrollback. */
    if (live_open_block && live_idx != SIZE_MAX && live_idx >= from) {
        size_t s = 0;
        size_t e = 0;
        msg_line_range(live_idx - from, &s, &e);
        size_t total_lines = e - s;
        size_t avail = (size_t)(rows - dock_n);
        if (avail < 2) {
            avail = 2;
        }
        live_keep = (total_lines <= avail) ? total_lines : avail;
    }

    /* ---- vorherigen live+dock-bereich raeumen: der cursor parkt
     *      nach jedem frame auf der LETZTEN dock-zeile, die
     *      g_prev_rows zeilen des bereichs liegen also BIS
     *      EINSCHLIESSLICH des cursors – hochgezogen wird darum
     *      g_prev_rows-1, nicht g_prev_rows: eine zeile zu weit
     *      wuerde die letzte content-zeile loeschen (die frisch
     *      gesendete nachricht!) */
    if (g_prev_rows > 0) {
        int up = g_prev_rows - 1;
        if (up > 0) {
            flush_up(up);
        }
        for (int i = 0; i < g_prev_rows; i++) {
            flush_puts("\r\x1b[K");
            if (i + 1 < g_prev_rows) {
                flush_put("\n", 1);
            }
        }
        if (up > 0) {
            flush_up(up);
        }
    }

    /* ---- content-zeilen: einmal drucken, dann gehoeren sie dem
     *      scrollback. die live-nachricht committet alle zeilen
     *      bis auf ihre letzte – die bleibt unten und waechst ---- */
    for (size_t mi = from; mi < chat->len; mi++) {
        size_t s = 0;
        size_t e = 0;
        msg_line_range(mi - from, &s, &e);
        size_t total = e - s;
        size_t start = 0;
        if (g_live_msg_valid && mi == g_live_msg) {
            /* war schon live: committete zeilen nicht erneut drucken */
            start = g_live_lines;
            if (start > total) {
                start = total;
            }
        }
        bool is_live = (mi == live_idx);
        size_t stop = total;
        if (is_live && total > 0 && !live_open_block) {
            stop = total - 1;
        }
        if (is_live && live_open_block) {
            /* block offen: committet wird hoechstens bis zur
             * zeile VOR dem block-anfang. der commit-offset darf
             * nie IN den block hineinwachsen – ein block, der
             * nach einem commit beginnt (streaming: der fence
             * kommt erst mit einem spaeteren chunk), haette
             * sonst schon committete zeilen im scrollback, die
             * beim block-wachsen nicht mehr mitfaerben koennen.
             * die live-region umfasst block + budget. */
            size_t blk_first = total; /* erste zeile des blocks */
            if (g_msg_blocks_valid && g_msg_blocks.n > 0) {
                const MdBlock *blk = &g_msg_blocks.blocks[g_msg_blocks.n - 1];
                for (size_t li = s; li < e; li++) {
                    if (g_lines[li].tool == -2 ||
                        (g_lines[li].lstart && g_lines[li].off >= blk->start &&
                         g_lines[li].off < blk->end)) {
                        blk_first = li - s;
                        break;
                    }
                }
            }
            /* commit-grenze: nie IN einen block, und NIE
             * r ueber committetes zurueck (g_live_lines ist
             * monoton – ein rollback wuerde zeilen doppelt
             * drucken). */
            if (blk_first < g_live_lines) {
                /* der block begann nach einem frueheren commit:
                 * diese zeilen stehen schon im scrollback. wir
                 * koennen sie nicht mehr live machen – der block
                 * committet also mit (halb fertige tabelle im
                 * scrollback ist besser als doppelt druck). */
                stop = total - 1; /* letzte zeile bleibt live */
            } else {
                stop = blk_first;
                if (total - stop > live_keep) {
                    /* budget: live-region zu gross – weitere
                     * zeilen vor dem block committen */
                    size_t extra = total - stop - live_keep;
                    if ((size_t)stop > extra) {
                        stop -= extra;
                    } else {
                        stop = 0;
                    }
                    if (stop < g_live_lines) {
                        stop = g_live_lines; /* monoton halten */
                    }
                }
            }
            g_live_msg = mi;
            g_live_msg_valid = true;
            g_live_lines = stop; /* committete zeilen */
        }
        /* die erste VORHANDENE zeile der nachricht (separator-
         *anker). eine leere assistant-nachricht mit tool-calls
         * rendert ihre text-zeile nicht – das "ai"-label wandert
         * auf die zeile des ersten calls */
        const ChatMessage *m = &chat->msgs[mi];
        size_t msg_first = s;
        if (m->role == CHAT_ROLE_ASSISTANT && m->text[0] == '\0' &&
            m->tool_calls_len > 0 && total > 0) {
            msg_first = s + 1;
        }

        /* die KLEMME: ist die nachricht hoeher als 75% des fensters,
         * fallen die unteren zeilen weg. sichtbar bleiben die ersten
         * `limit` zeilen plus die LETZTE (exit-code / stream-ende);
         * dazwischen steht der graue hinweis. live-nachrichten
         * klemmen genauso – die wachsende zeile ist die letzte und
         * bleibt. die klemme wird je frame neu berechnet, ein
         * resize wirkt sofort. */
        size_t clamped = 0; /* zeilen, die wegfallen (inkl. der
                             * letzten, die separat gezeigt wird) */
        {
            /* budget = 75% der fensterhoehe. gerendert: budget-1
             * content-zeilen oben (head), der hinweis, die letzte
             * zeile. clamped = alles zwischen head und der letzten
             * (exklusiv) – die loop rendert head zeilen, der
             * hinweis zeigt clamped-1 "weitere" + die letzte zeile
             * als bonus. mindest-budget 3 (2 content + hinweis),
             * damit winzige fenster die nachricht nicht fressen. */
            int budget = (rows * MSG_MAX_PCT) / 100;
            if (budget < 3) {
                budget = 3;
            }
            size_t head = (size_t)budget - 1;
            size_t visible = stop - start;
            if (visible > head + 1) {
                clamped = visible - head;
            }
        }

        for (size_t li = s + start; li + clamped < s + stop; li++) {
            /* leerzeile zwischen den nachrichten: sie gehoert zum
             * FERTIGEN content (druckt einmal, scrollt dann mit).
             * `li == msg_first` feuert nur bei der ersten GEDRUCK-
             * TEN zeile einer nachricht; bei der live-nachricht
             * mit start > 0 ist li > msg_first – der abstand kam
             * frueher, beim commit der ersten zeile. */
            if (li == msg_first && g_printed_any) {
                row_start(main_w);
                row_finish(true);
            }
            const ChatLine *ln = &g_lines[li];
            const ChatMessage *lm = &chat->msgs[ln->msg + from];

            if (ln->tool == -2) {
                /* tabellen-zeile: off/len zeigen in den tabellen-
                 * display-string (md_table_display), nicht in den
                 * nachricht-text. der inhalt ist linksbuendig
                 * ausgerichtet, die pipes farbig. der string ist
                 * pro frame frisch – chat_wrap baut ihn beim
                 * wrappen. hier drucken wir die zeile STUECKWEISE:
                 * pipes in akzentfarbe, zellen normal. */
                row_start(main_w);
                row_sgr(&g_row, THEME_ROLE_RESET);
                {
                    /* display-string fuer diese tabelle neu bauen
                     * (gleicher algorithmus wie chat_wrap: die
                     * zeilen MUessen identisch sein). blk_start/end
                     * sind KOMPIE-offsets, md_table_display will
                     * sie relativ zum text der nachricht */
                    size_t toff = chat_disp_off(ln->msg);
                    char *tbl =
                        md_table_display(disp + toff, ln->blk_start - toff,
                                         ln->blk_end - toff, main_w);
                    if (tbl != NULL) {
                        size_t dl = strlen(tbl);
                        size_t off = ln->off;
                        if (off < dl) {
                            size_t b = 0;
                            while (b < ln->len && off + b < dl) {
                                /* ganzes utf-8-zeichen: ein codepoint
                                 * ist eine zelle, umlaute duerfen nicht
                                 * je byte zaehlen */
                                size_t clen = row_utf8_step(tbl + off + b);
                                if (off + b + clen > dl || b + clen > ln->len) {
                                    clen = 1; /* rand: nur das byte */
                                }
                                if (tbl[off + b] == '|') {
                                    row_sgr(&g_row, theme_current()->match);
                                }
                                row_putn(&g_row, tbl + off + b, (int)clen);
                                if (tbl[off + b] == '|') {
                                    row_sgr(&g_row, THEME_ROLE_RESET);
                                }
                                b += clen;
                            }
                        }
                        free(tbl);
                    }
                }
                row_sgr(&g_row, THEME_ROLE_RESET);
                row_finish(true);
            } else if (ln->tool >= 0) {
                /* tool-call-zeile: bei einer nachricht OHNE text
                 * (die ki hat nur tools aufgerufen) steht das "ai"-
                 * label direkt am ersten call – eine eigene zeile
                 * nur fuer das label gibt es nicht */
                row_start(main_w);
                for (int i = 0; i < MSG_PAD_W; i++) {
                    row_putc(&g_row, ' '); /* padding wie text */
                }
                row_tool_call(&g_row, &lm->tool_calls[ln->tool], ln);
                row_sgr(&g_row, THEME_ROLE_RESET);
                row_finish(true);
            } else if (ln->tool == -3) {
                /* thinking-zeile (reasoning des modells): dim, ohne
                 * markdown. die erste bekommt ein "thinking:"-label,
                 * damit im scrollback klar ist, was hier steht –
                 * ansonsten waere es von einer antwort nicht zu
                 * unterscheiden */
                if (ln->first) {
                    row_start(main_w);
                    row_sgr(&g_row, theme_role(THEME_ROLE_DIM));
                    row_puts(&g_row, " thinking:");
                    row_sgr(&g_row, THEME_ROLE_RESET);
                    row_finish(true);
                }
                row_start(main_w);
                row_putc(&g_row, ' '); /* padding wie text */
                row_sgr(&g_row, theme_role(THEME_ROLE_DIM));
                row_putn(&g_row, disp + ln->off, (int)ln->len);
                row_sgr(&g_row, THEME_ROLE_RESET);
                row_finish(true);
            } else if (lm->role == CHAT_ROLE_TOOL) {
                /* tool-ergebnis: "output:" vor der ersten zeile,
                 * dahinter der echte output, am ende der (von
                 * bash angehaengte) exit-code gruen/rot. leer-
                 * zeilen VOR dem exit-code ueberliest der wrap-
                 * pass (chat_wrap) – der code klebt direkt am
                 * letzten output */
                if (ln->first) {
                    row_start(main_w);
                    row_sgr(&g_row, theme_role(THEME_ROLE_DIM));
                    row_puts(&g_row, " output:");
                    row_sgr(&g_row, THEME_ROLE_RESET);
                    row_finish(true);
                }
                row_start(main_w);
                int code = exit_marker_of(disp, ln);
                if (code >= 0) {
                    row_puts(&g_row, " ");
                    row_sgr(&g_row,
                            theme_role((code == 0) ? THEME_ROLE_MD_OK
                                                   : THEME_ROLE_MD_ERR));
                    row_putn(&g_row, disp + ln->off, (int)ln->len);
                    row_sgr(&g_row, THEME_ROLE_RESET);
                } else {
                    row_puts(&g_row, " ");
                    row_putn(&g_row, disp + ln->off, (int)ln->len);
                }
                row_sgr(&g_row, THEME_ROLE_RESET);
                row_finish(true);
            } else if (m->role == CHAT_ROLE_ASSISTANT && m->text[0] == '\0' &&
                       m->tool_calls_len > 0 && ln->first) {
                /* leere text-zeile einer call-nachricht: uebersprun-
                 * gen, das label sitzt am ersten call */
                continue;
            } else {
                row_start(main_w);
                row_msg(&g_row, ln, disp);
                row_finish(true);
            }
            g_printed_any = true;
        }

        /* klemmen-hinweis: unter den sichtbaren zeilen, in grau
         * (THEME_ROLE_MORE), mit dem 1er-padding links wie alle
         * nachrichtenzeilen */
        if (clamped > 0) {
            row_start(main_w);
            row_sgr(&g_row, theme_role(THEME_ROLE_MORE));
            char more[64];
            (void)snprintf(more, sizeof more,
                           " \xE2\x80\xA6 und %zu weitere zeilen", clamped - 1);
            row_puts(&g_row, more);
            row_sgr(&g_row, THEME_ROLE_RESET);
            row_finish(true);
            g_printed_any = true;
        }

        /* die weggeklemmte LETZTE zeile bleibt immer sichtbar:
         * der exit-code eines tool-outputs haengt dort, das
         * stream-ende waechst dort */
        if (clamped > 0 && stop > start) {
            const ChatLine *ln = &g_lines[s + stop - 1];
            const ChatMessage *lm = &chat->msgs[ln->msg + from];
            row_start(main_w);
            if (lm->role == CHAT_ROLE_TOOL) {
                int code = exit_marker_of(disp, ln);
                row_puts(&g_row, " ");
                if (code >= 0) {
                    row_sgr(&g_row,
                            theme_role((code == 0) ? THEME_ROLE_MD_OK
                                                   : THEME_ROLE_MD_ERR));
                }
                row_putn(&g_row, disp + ln->off, (int)ln->len);
                row_sgr(&g_row, THEME_ROLE_RESET);
            } else {
                row_msg(&g_row, ln, disp);
            }
            row_finish(true);
            g_printed_any = true;
        }

        if (is_live) {
            g_live_msg = mi;
            g_live_msg_valid = true;
            g_live_lines = stop;
        }
    }
    if (live_idx != SIZE_MAX) {
        g_printed = live_idx; /* live-nachricht ist die frontier */
    } else {
        /* turn vorbei: die live-nachricht ist fertig und wird im
         * naechsten frame GANZ gedruckt (g_live_lines 0) – so
         * bekommt die tabelle ihre finalen breiten. der frame
         * nach dem turn raeumt die alte live-region (g_prev_rows
         * stimmt vom letzten frame) und druckt die nachricht
         * komplett neu. */
        g_printed = chat->len;
        g_live_msg_valid = false;
        g_live_lines = 0;
    }

    /* ---- live-zeile(n): das wachsende stream-ende. das "thinking"
     *      lebt seit der spinner-umstellung in der rahmenzeile des
     *      eingabefelds – ohne text gibt es hier KEINE zeile.
     *      bei offenem block (fence/tabelle) sind ALLE zeilen der
     *      live-nachricht live: sie werden je frame komplett neu
     *      gezeichnet (breiten wachsen mit), gemaess der
     *      live_open_block-regel weiter oben committet nichts. */
    bool live_text = false;
    if (state->busy && live_idx != SIZE_MAX && from <= live_idx &&
        disp[chat_disp_off(live_idx - from)] != '\0') {
        size_t s = 0;
        size_t e = 0;
        msg_line_range(live_idx - from, &s, &e);
        if (e > s) {
            if (live_open_block) {
                /* live ab dem commit-punkt (der content-loop
                 * hat alles davor gedruckt) bis zum ende; das
                 * budget begrenzt von unten, committete zeilen
                 * werden nie doppelt gezeichnet. */
                size_t first_live = e - live_keep;
                if (first_live < s) {
                    first_live = s;
                }
                if (g_live_msg_valid && g_live_msg == live_idx &&
                    first_live < s + g_live_lines) {
                    first_live = s + g_live_lines;
                }
                for (size_t li = first_live; li < e; li++) {
                    const ChatLine *ln = &g_lines[li];
                    if (ln->tool >= 0) {
                        continue;
                    }
                    row_start(main_w);
                    if (ln->tool == -3) {
                        /* thinking live: dim, wie im content-loop */
                        row_putc(&g_row, ' ');
                        row_sgr(&g_row, theme_role(THEME_ROLE_DIM));
                        row_putn(&g_row, disp + ln->off, (int)ln->len);
                        row_sgr(&g_row, THEME_ROLE_RESET);
                    } else if (ln->tool == -2) {
                        /* tabelle: wie im content-loop (pipes farbig).
                         * blk_start/end sind kopie-offsets */
                        size_t toff = chat_disp_off(ln->msg);
                        char *tbl =
                            md_table_display(disp + toff, ln->blk_start - toff,
                                             ln->blk_end - toff, main_w);
                        if (tbl != NULL) {
                            size_t dl = strlen(tbl);
                            if (ln->off < dl) {
                                size_t b = 0;
                                while (b < ln->len && ln->off + b < dl) {
                                    size_t clen =
                                        row_utf8_step(tbl + ln->off + b);
                                    if (ln->off + b + clen > dl ||
                                        b + clen > ln->len) {
                                        clen = 1;
                                    }
                                    if (tbl[ln->off + b] == '|') {
                                        row_sgr(&g_row, theme_current()->match);
                                    }
                                    row_putn(&g_row, tbl + ln->off + b,
                                             (int)clen);
                                    if (tbl[ln->off + b] == '|') {
                                        row_sgr(&g_row, THEME_ROLE_RESET);
                                    }
                                    b += clen;
                                }
                            }
                            free(tbl);
                        }
                    } else {
                        row_msg(&g_row, ln, disp);
                    }
                    row_finish(true);
                }
                live_text = true;
            } else {
                const ChatLine *ln = &g_lines[e - 1];
                if (ln->tool < 0) {
                    row_start(main_w);
                    if (ln->tool == -3) {
                        row_putc(&g_row, ' ');
                        row_sgr(&g_row, theme_role(THEME_ROLE_DIM));
                        row_putn(&g_row, disp + ln->off, (int)ln->len);
                        row_sgr(&g_row, THEME_ROLE_RESET);
                    } else {
                        row_msg(&g_row, ln, disp);
                    }
                    row_finish(true);
                    live_text = true;
                }
            }
        }
    }
    bool tool_spin = false;
    if (state->busy && state->tool_running) {
        long long busy_ms =
            (state->busy_start_ms > 0) ? mono_ms() - state->busy_start_ms : 0;
        row_start(main_w);
        row_tool_spinner(&g_row, busy_ms);
        row_finish(true);
        tool_spin = true;
    }

    /* ---- dock (immer die letzten zeilen des frames) ---- */
    for (int i = 0; i < dock_n; i++) {
        row_start(main_w);
        render_dock_row(&dock[i], &d, state, cfg);
        row_finish(i + 1 < dock_n);
    }

    /* der naechste frame raeumt genau diesen bereich: live-zeile
     * (falls eine gedruckt wurde) plus dock. bei offenem block
     * umfasst die live-region ALLE zeilen der live-nachricht –
     * der erase des naechsten frames muss sie alle raeumen. */
    int live_rows = 0;
    if (live_open_block && live_idx != SIZE_MAX) {
        /* offener block: die live-region umfasst genau die zeilen,
         * die der live-block GEDRUCKT hat (first_live..e), nicht
         * die ganze nachricht – committete zeilen stehen im
         * scrollback und gehoeren nicht zur ueberschreibbaren
         * region. tool-spinner zaehlt darunter extra. */
        size_t s = 0;
        size_t e = 0;
        msg_line_range(live_idx - from, &s, &e);
        size_t first_live = e - live_keep;
        if (first_live < s) {
            first_live = s;
        }
        if (g_live_msg_valid && g_live_msg == live_idx &&
            first_live < s + g_live_lines) {
            first_live = s + g_live_lines;
        }
        live_rows = (int)(e - first_live);
    } else if (live_text || tool_spin) {
        live_rows = 1;
    }
    g_prev_rows = dock_n + live_rows;
    /* anker fuer den leichten tick (draw_busy_tick): nur der tool-
     * spinner im chat ist zeitgetrieben; das stream-ende aendert
     * sich nur mit chunks (voller frame). der cursor parkt AUF der
     * letzten dock-zeile: die live-zeile liegt g_prev_rows-1
     * darueber (wie im erase-block) */
    g_tool_spin_up = 0;
    if (tool_spin) {
        g_tool_spin_up = g_prev_rows - 1;
    }
    /* live-region gewachsen? dann ist der leichte tick bis zum
     * naechsten vollen frame verboten: sein anker (g_busy_up)
     * haelt den ABSTAND des spinner-rahmens vom cursor fest – hat
     * die live-region zeilen dazugewonnen, liegt der rahmen beim
     * tick um genau die differenz zu HOCH und ueberschreibt
     * content-zeilen (doppelte zeilen im stream). der volle frame
     * danach setzt den anker neu. */
    static int prev_live_rows = -1;
    g_busy_up = 0;
    if (state->busy && !(live_open_block && live_rows > 0 &&
                         prev_live_rows >= 0 && live_rows != prev_live_rows)) {
        g_busy_up = d.busy_row_up;
    }
    prev_live_rows = live_rows;

    dbg("frame: content+live=%d dock=%d printed=%zu", live_rows, dock_n,
        g_printed);

    /* der ganze frame als EIN write: stdout am terminal ist
     * zeilen-gepuffert, ohne den sammler flackerte das geraeumte
     * dock bei jedem tastendruck zwischen erase und neuzeichnen.
     * der flush garantiert zusaetzlich, dass auch die letzte
     * dock-zeile (ohne umbruch) wirklich draussen ist. */
    flush_out();
}

/* leichter frame, solange die ki arbeitet: nur die zeitgetriebenen
 * zeilen (spinner-rahmen, tool-spinner im chat) werden an ort und
 * stelle ueberschrieben – der rest des docks bleibt unberuehrt.
 * der watchdog feuert ~10 ticks/s; ein VOLLER frame je tick
 * liesse die ganze input-leiste flackern. bezugspunkt ist die
 * geparkte cursor-position aus dem letzten vollen frame; nach
 * resize sind die anker 0 und es wird doch voll gezeichnet. */
void draw_busy_tick(int rows, int cols, AppState *state, const Config *cfg)
{
    if (!state->busy || g_busy_up <= 0) {
        draw(rows, cols, state, cfg);
        return;
    }
    int main_w = main_width(cols);
    long long busy_ms =
        (state->busy_start_ms > 0) ? mono_ms() - state->busy_start_ms : 0;

    if (g_tool_spin_up > 0) {
        /* chat-spinner unter dem call: direkt ueber dem dock */
        flush_up(g_tool_spin_up);
        row_start(main_w);
        row_tool_spinner(&g_row, busy_ms);
        row_finish(false);
        flush_down(g_tool_spin_up - g_busy_up);
    } else {
        flush_up(g_busy_up);
    }

    row_start(main_w);
    row_busy_border(&g_row, busy_ms);
    row_finish(false);
    flush_down(g_busy_up);
    flush_out();
}
