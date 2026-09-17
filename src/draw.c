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
#include "send.h"
#include "session.h"
#include "settings.h"
#include "theme.h"
#include "utils.h"

static const char GLYPH_EM_DASH[] = "\xE2\x80\x94";
static const char GLYPH_BLOCK[] = "\xE2\x96\x88";

/* breite des zeilen-labels ("you  ", "ai   ", "tool ", ...) bzw.
 * der einrueckung von folgezeilen und tool-call-zeilen: alle
 * nachrichtentexte starten damit in derselben spalte */
#define MSG_PREFIX_W 5

/* kein eingabe-praefix mehr: der text beginnt bei spalte 0 –
 * diese spalte bleibt fuer den cursor-block am zeilenende frei */
#define INPUT_PREFIX_W 0

/* die zwei statuszeilen am unteren rand des docks */
#define STATUS_H 2

/* maximalzeilen des docks: quit + cmds + eingabe-rahmen + dialog +
 * status – der dock klemmt ohnehin am terminal */
#define DOCK_ROWS_MAX 96

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
static void row_putc(Row *r, char c);
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
static void row_prompt_opt(Row *r, int idx, const char *search, bool selected,
                           bool active, int id_col);
static void row_prompt_hint(Row *r);
static void row_session(Row *r, const SessionInfo *si, const char *search,
                        bool selected, bool active, int id_col, int name_col);
static void row_command(Row *r, const Command *cmd, const char *prefix);
static void row_no_match(Row *r, const char *prefix);
static void row_status_model(Row *r, const Config *cfg);
static void row_status_tokens(Row *r, const AppState *st);
static void row_quit(Row *r);
static void row_msg(Row *r, const ChatLine *ln, const char *text);
static void row_tool_call(Row *r, const ChatToolCall *call, const ChatLine *ln);
static void row_busy_border(Row *r, long long busy_ms);
static void row_tool_spinner(Row *r, long long busy_ms);
static int exit_marker_of(const char *text, const ChatLine *ln);
static void fmt_dur(long long ms, char *buf, size_t sz);
static void fmt_when(long long ms, char *buf, size_t sz);

static void row_pad(Row *r)
{
    while (r->cells < r->width) {
        row_putc(r, ' ');
    }
}

static void row_putc(Row *r, char c)
{
    if (r->cells >= r->width) {
        return;
    }
    r->f->buf[r->f->pos++] = c;
    r->cells++;
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

/* die ersten n byte von s, am rand abgeschnitten */
static void row_putn(Row *r, const char *s, int n)
{
    for (int i = 0; i < n && s[i] != '\0'; i++) {
        row_putc(r, s[i]);
    }
}

static void row_puts(Row *r, const char *s)
{
    while (*s != '\0') {
        row_putc(r, *s++);
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
    case SET_SYSTEM_PROMPT:
        if (cfg->system_prompt == NULL) {
            row_puts(r, "default");
        } else if (cfg->system_prompt[0] == '\0') {
            row_puts(r, "off");
        } else {
            row_puts(r, cfg->system_prompt);
        }
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

/* eintrag des system-prompt-untermenues. "(active)" haengt an der
 * option, die dem aktuellen config-zustand entspricht. */
static void row_prompt_opt(Row *r, int idx, const char *search, bool selected,
                           bool active, int id_col)
{
    const Theme *theme = theme_current();
    if (idx < 0 || idx >= PROMPT_OPT_COUNT) {
        return;
    }
    const char *name = PROMPT_OPT_NAMES[idx];

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

/* hinweis unter dem eingabefeld, solange es den system-prompt
 * bearbeitet: enter speichert hier statt zu senden. */
static void row_prompt_hint(Row *r)
{
    row_sgr(r, theme_role(THEME_ROLE_DIM));
    row_puts(r, "  system prompt: enter speichert, esc verwirft");
    row_sgr(r, THEME_ROLE_RESET);
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

/* token-zahlen kurz halten: 1234 -> "1.2k", 45678 -> "45k" */
static void put_count(Row *r, size_t n)
{
    char buf[32];
    if (n < 1000) {
        (void)snprintf(buf, sizeof buf, "%zu", n);
    } else if (n < 100000) {
        (void)snprintf(buf, sizeof buf, "%zu.%zuk", n / 1000, (n % 1000) / 100);
    } else {
        (void)snprintf(buf, sizeof buf, "%zuk", n / 1000);
    }
    row_puts(r, buf);
}

static void status_label(Row *r, const char *label)
{
    row_sgr(r, theme_role(THEME_ROLE_DIM));
    row_puts(r, " ");
    row_puts(r, label);
    row_sgr(r, THEME_ROLE_RESET);
}

/* statuszeile 1: mit wem wir sprechen und wie gross dessen fenster
 * ist. */
static void row_status_model(Row *r, const Config *cfg)
{
    status_label(r, "model   ");

    const Provider *provider = NULL;
    const Model *m = send_find_model(cfg, cfg->active_model, &provider);
    if (m == NULL || m->id == NULL) {
        row_sgr(r, theme_role(THEME_ROLE_DIM));
        row_puts(r, "keins gewaehlt \xE2\x80\x93 /models");
        row_sgr(r, THEME_ROLE_RESET);
        return;
    }

    row_sgr(r, theme_role(THEME_ROLE_ASSISTANT));
    row_puts(r, m->id);
    row_sgr(r, THEME_ROLE_RESET);

    row_sgr(r, theme_role(THEME_ROLE_DIM));
    if (m->context_window > 0) {
        row_puts(r, "  \xC2\xB7  ");
        put_count(r, m->context_window);
        row_puts(r, " kontext");
    }
    if (provider != NULL && provider->base_url != NULL) {
        row_puts(r, "  \xC2\xB7  ");
        row_puts(r, provider->base_url);
    }
    row_sgr(r, THEME_ROLE_RESET);
}

/* statuszeile 2: was die sitzung gekostet hat, rechts daneben name
 * und id der offenen session. */
static void row_status_tokens(Row *r, const AppState *st)
{
    status_label(r, "tokens  ");

    row_sgr(r, theme_role(THEME_ROLE_DIM));
    put_count(r, st->ctx.total_prompt);
    row_puts(r, " gesendet  \xC2\xB7  ");
    put_count(r, st->ctx.total_completion);
    row_puts(r, " empfangen");

    /* gesamt-arbeitszeit der session: thinking, tool calls,
     * antworten – alles, was die ki gearbeitet hat */
    if (st->worked_ms > 0) {
        char dur[24];
        fmt_dur(st->worked_ms, dur, sizeof dur);
        row_puts(r, "  \xC2\xB7  arbeit ");
        row_puts(r, dur);
    }

    if (st->session.active) {
        row_puts(r, "  \xC2\xB7  session ");
        if (st->session.name != NULL) {
            row_sgr(r, THEME_ROLE_RESET);
            row_sgr(r, theme_role(THEME_ROLE_ASSISTANT));
            row_puts(r, st->session.name);
            row_sgr(r, THEME_ROLE_RESET);
            row_sgr(r, theme_role(THEME_ROLE_DIM));
            row_puts(r, "  \xC2\xB7  ");
        }
        row_puts(r, st->session.id);
    }

    /* --debug: der trace landet in einer datei unter /tmp – deren
     * name steht hier, damit mensch und agent ihn finden. dbg_path
     * ist erst nach dem start gesetzt und aendert sich, sobald die
     * erste nachricht die session (und damit den umbenannten log)
     * oeffnet: die zeile aktualisiert sich von selbst */
    if (dbg_active()) {
        row_puts(r, "  \xC2\xB7  debug mode, log file: ");
        row_puts(r, dbg_path());
    }
    row_sgr(r, THEME_ROLE_RESET);
}

static void row_quit(Row *r)
{
    row_puts(r, "quit? ctrl+c again to confirm");
}

/* label und farbe einer rolle: alle labels sind MSG_PREFIX_W breit,
 * die texte aller rollen starten damit in derselben spalte –
 * "you" fuer den benutzer, "ai" fuer die antwort des modells. */
static ThemeRole role_theme(ChatRole role, const char **label)
{
    switch (role) {
    case CHAT_ROLE_USER:
        *label = "you  ";
        return THEME_ROLE_USER;
    case CHAT_ROLE_ASSISTANT:
        *label = "ai   ";
        return THEME_ROLE_ASSISTANT;
    case CHAT_ROLE_ERROR:
        *label = "err  ";
        return THEME_ROLE_ERROR;
    case CHAT_ROLE_TOOL:
        *label = "tool ";
        return THEME_ROLE_TOOL;
    case CHAT_ROLE_NOTICE:
        *label = "ctx  ";
        return THEME_ROLE_NOTICE;
    case CHAT_ROLE_SYSTEM:
        break;
    }
    *label = "sys  ";
    return THEME_ROLE_SYSTEM;
}

static void row_msg(Row *r, const ChatLine *ln, const char *text)
{
    if (ln->first) {
        const char *label = NULL;
        ThemeRole role = role_theme(ln->role, &label);
        row_sgr(r, theme_role(role));
        row_puts(r, label);
        row_sgr(r, THEME_ROLE_RESET);
    } else {
        row_puts(r, "     "); /* umbruch-folgezeile: unter dem label */
    }
    row_putn(r, text + ln->off, (int)ln->len);
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
    row_puts(r, "     "); /* auf spalte MSG_PREFIX_W, wie der output */
    row_sgr(r, theme_role(THEME_ROLE_DIM));
    row_glyph(r, SPIN[spin_frame(busy_ms)]);
    row_sgr(r, THEME_ROLE_RESET);
}

/* "[exit: N]": der bash-anhang. rueckgabe: N, oder -1 wenn dieser
 * zeilenabschnitt KEIN exit-marker ist. die faerbung geschieht im
 * renderer (gruen/rot); das model und das session-log sehen den
 * text weiterhin unfaerbt. */
static int exit_marker_of(const char *text, const ChatLine *ln)
{
    const char *s = text + ln->off;
    size_t len = ln->len;
    /* "[exit: N]": 7 zeichen rahmen + 1..3 ziffern */
    if (len < 8 || len > 10) {
        return -1;
    }
    if (s[0] != '[' || s[6] != ' ' || s[len - 1] != ']') {
        return -1;
    }
    if (strncmp(s, "[exit", 5) != 0) {
        return -1;
    }
    int code = 0;
    for (size_t i = 7; i + 1 < len; i++) {
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
    size_t need = ((size_t)main_w * 4) + 64;
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

void draw_reset(int rows)
{
    dbg("draw: reset (%d zeilen scrollen)", rows);
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
    DROW_PROMPT_HINT,   /* hinweis: eingabe bearbeitet den prompt */
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
    int sel_idx;       /* flacher index des angewaehlten eintrags */
    long long busy_ms; /* laufende arbeitszeit des turns (spinner) */
    int busy_row_up;   /* spinner-rahmen: zeilen ueber dem geparkten
                        * cursor (anker fuer den leichten tick) */
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
    case MODE_PROMPT:
        return "prompt";
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
    case MODE_PROMPT:
        snprintf(d->search, sizeof d->search, "%s", st->dialog.search);
        d->match_count =
            names_match(PROMPT_OPT_NAMES, PROMPT_OPT_COUNT, st->dialog.search,
                        matches, DIALOG_MATCH_MAX);
        for (int i = 0; i < d->match_count; i++) {
            d->match_idx[i] = matches[i];
        }
        d->id_col = names_col(PROMPT_OPT_NAMES, PROMPT_OPT_COUNT);
        break;
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
    int usable = rows - STATUS_H;
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
        if (st->prompt_edit && list_h == 0) {
            list_h = 1; /* der hinweis steht, wo die cmd-liste stuende */
        }
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
        } else if (st->prompt_edit && n < out_max) {
            out_rows[n++] = (DockRow){DROW_PROMPT_HINT, 0, false};
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

    if (n < out_max) {
        out_rows[n++] = (DockRow){DROW_STATUS_MODEL, 0, false};
    }
    if (n < out_max) {
        out_rows[n++] = (DockRow){DROW_STATUS_TOKENS, 0, false};
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
    case DROW_PROMPT_HINT:
        row_prompt_hint(r);
        break;
    case DROW_QUIT:
        row_quit(r);
        break;
    case DROW_STATUS_MODEL:
        row_status_model(r, cfg);
        break;
    case DROW_STATUS_TOKENS:
        row_status_tokens(r, st);
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
        case MODE_PROMPT: {
            bool active = false;
            if (row->a == PROMPT_DEFAULT) {
                active = (cfg->system_prompt == NULL);
            } else if (row->a == PROMPT_OFF && cfg->system_prompt != NULL &&
                       cfg->system_prompt[0] == '\0') {
                active = true;
            }
            row_prompt_opt(r, row->a, d->search, row->sel, active, d->id_col);
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
    int text_w = main_w - MSG_PREFIX_W;
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
    if (state->busy && chat->len > 0 &&
        chat->msgs[chat->len - 1].role == CHAT_ROLE_ASSISTANT &&
        chat->msgs[chat->len - 1].tool_calls_len == 0) {
        live_idx = chat->len - 1;
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
        if (is_live && total > 0) {
            stop = total - 1;
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

        for (size_t li = s + start; li < s + stop; li++) {
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

            if (ln->tool >= 0) {
                /* tool-call-zeile: bei einer nachricht OHNE text
                 * (die ki hat nur tools aufgerufen) steht das "ai"-
                 * label direkt am ersten call – eine eigene zeile
                 * nur fuer das label gibt es nicht */
                row_start(main_w);
                if (lm->role == CHAT_ROLE_ASSISTANT && lm->text[0] == '\0' &&
                    ln->tool == 0 && ln->off == 0) {
                    row_sgr(&g_row, theme_role(THEME_ROLE_ASSISTANT));
                    row_puts(&g_row, "ai   ");
                    row_sgr(&g_row, THEME_ROLE_RESET);
                } else {
                    for (int i = 0; i < MSG_PREFIX_W; i++) {
                        row_putc(&g_row, ' ');
                    }
                }
                row_tool_call(&g_row, &lm->tool_calls[ln->tool], ln);
                row_finish(true);
            } else if (lm->role == CHAT_ROLE_TOOL) {
                /* tool-ergebnis: "output:" vor der ersten zeile,
                 * dahinter der echte output, am ende der (von
                 * bash angehaengte) exit-code gruen/rot */
                if (ln->first) {
                    row_start(main_w);
                    row_sgr(&g_row, theme_role(THEME_ROLE_DIM));
                    row_puts(&g_row, "     output:");
                    row_sgr(&g_row, THEME_ROLE_RESET);
                    row_finish(true);
                }
                row_start(main_w);
                int code = exit_marker_of(lm->text, ln);
                if (code >= 0) {
                    row_puts(&g_row, "     ");
                    row_sgr(&g_row, (code == 0) ? "\x1b[32m" : "\x1b[31m");
                    row_putn(&g_row, lm->text + ln->off, (int)ln->len);
                    row_sgr(&g_row, THEME_ROLE_RESET);
                } else {
                    row_puts(&g_row, "     ");
                    row_putn(&g_row, lm->text + ln->off, (int)ln->len);
                }
                row_finish(true);
            } else if (m->role == CHAT_ROLE_ASSISTANT && m->text[0] == '\0' &&
                       m->tool_calls_len > 0 && ln->first) {
                /* leere text-zeile einer call-nachricht: uebersprun-
                 * gen, das label sitzt am ersten call */
                continue;
            } else {
                row_start(main_w);
                row_msg(&g_row, ln, lm->text);
                row_finish(true);
            }
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
        g_printed = chat->len;
        g_live_msg_valid = false;
        g_live_lines = 0;
    }

    /* ---- live-zeile: das wachsende stream-ende. das "thinking"
     *      lebt seit der spinner-umstellung in der rahmenzeile des
     *      eingabefelds – ohne text gibt es hier KEINE zeile ---- */
    bool live_text = false;
    if (state->busy && live_idx != SIZE_MAX &&
        chat->msgs[live_idx].text[0] != '\0') {
        size_t s = 0;
        size_t e = 0;
        msg_line_range(live_idx - from, &s, &e);
        if (e > s) {
            const ChatLine *ln = &g_lines[e - 1];
            if (ln->tool < 0) {
                row_start(main_w);
                row_msg(&g_row, ln, chat->msgs[live_idx].text);
                row_finish(true);
                live_text = true;
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
     * (falls eine gedruckt wurde) plus dock */
    int live_rows = 0;
    if (live_text || tool_spin) {
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
    g_busy_up = 0;
    if (state->busy) {
        g_busy_up = d.busy_row_up;
    }

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
