#include "draw.h"

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "chat.h"
#include "command.h"
#include "settings.h"
#include "state.h"
#include "theme.h"
#include "utils.h"

static const char GLYPH_EM_DASH[] = "\xE2\x80\x94";
static const char GLYPH_BLOCK[] = "\xE2\x96\x88";

/* breite des zeilen-labels ("you  ", "max  ", ...) bzw. der
 * einrueckung von folgezeilen */
#define MSG_PREFIX_W 5

/* ------------------------------------------------------------------ */
/* zeilen-arena fuer den chat: layout_compute fuellt sie jeden frame  */
/* neu (breite kann sich geaendert haben), der speicher bleibt ueber  */
/* frames erhalten. waechst nur, schrumpft nie – chat-verlauf lebt */
/* eh solange die app laeuft. */
/* ------------------------------------------------------------------ */
static ChatLine *g_chat_lines = NULL;
static size_t g_chat_lines_cap = 0;

static size_t setup_pos(char *buf)
{
    size_t res = 0;
    res += (size_t)sprintf(buf + res, "\x1b[H\x1b[m");
    return res;
}

static void frame_begin(Frame *f, int rows, int cols)
{
    /* grobzuegig bemessen: ansi-sequenzen + utf-8 brauchen deutlich
     * mehr bytes als sichtbare zellen, 4 bytes/zelle reicht immer */
    size_t need = (size_t)rows * ((((size_t)cols * 4) + 2) + 64);
    f->buf = malloc(need);
    if (!f->buf) {
        die("out of memory");
    }
    f->pos = setup_pos(f->buf);
}

static void frame_row_end(Frame *f)
{
    f->buf[f->pos++] = '\r';
    f->buf[f->pos++] = '\n';
}

static void frame_end(Frame *f, int rows)
{
    /* cursor an den anfang der letzten zeile setzen */
    f->pos += (size_t)sprintf(f->buf + f->pos, "\x1b[%d;1H", rows);
    fwrite(f->buf, 1, f->pos, stdout);
    fflush(stdout);
    free(f->buf);
}

void layout_dialog_box(Layout *lt, int rows, DialogState *d)
{
    /* eintrags-bereich: max 50% des bildschirms,
     * mind. 1 zeile (kein treffer: hinweis-zeile) */
    lt->entries_h = (lt->match_count > 0) ? lt->match_count : 1;
    if (lt->entries_h > rows / 2) {
        lt->entries_h = rows / 2;
    }
    if (lt->entries_h < 1) {
        lt->entries_h = 1;
    }

    /* cursor/scroll normalisieren: NUR hier sind trefferzahl und
     * sichtbare hoehe gemeinsam bekannt -> zurueckschreiben in den
     * state, damit die key-behandlung (enter) konsistente werte
     * sieht, egal ob ein draw dazwischen lag */
    if (d->selected >= lt->match_count) {
        d->selected = lt->match_count - 1; /* filter schrumpfte */
    }
    if (d->selected < 0) {
        d->selected = 0;
    }
    if (d->scroll + lt->entries_h > lt->match_count) {
        d->scroll = lt->match_count - lt->entries_h;
    }
    if (d->scroll < 0) {
        d->scroll = 0;
    }
    if (d->selected < d->scroll) {
        d->scroll = d->selected;
    }
    if (d->selected >= d->scroll + lt->entries_h) {
        d->scroll = d->selected - lt->entries_h + 1;
    }
    lt->scroll = d->scroll;
    lt->selected = (lt->match_count > 0) ? lt->match_idx[d->selected] : -1;

    /* box von unten: border, suchzeile, dann die eintraege */
    lt->box_top = rows - lt->entries_h - 2;
    lt->box_bottom = rows - 1;
}

void layout_compute(Layout *lt, int rows, int cols, UIMode mode, AppState *st,
                    const Config *cfg)
{
    memset(lt, 0, sizeof *lt);
    lt->mode = mode;
    lt->rows = rows;
    lt->main_w = main_width(cols);

    if (st->confirm_quit) {
        lt->quit_row = rows - 1;
    }

    switch (mode) {
    case MODE_INPUT:
        if (st->cmd_active) {
            cmd_prefix(&st->input, lt->prefix, sizeof lt->prefix);
            lt->match_count =
                cmd_match(lt->prefix, lt->match_idx, COMMAND_COUNT);
            /* kein treffer: eine zeile fuer den hinweis reservieren */
            lt->cmd_h = (lt->match_count > 0) ? lt->match_count : 1;
        }
        lt->input_bottom = bottom_border_for(rows, lt->cmd_h, st->confirm_quit);
        lt->input_top = lt->input_bottom - (int)st->input.count - 1;
        if (lt->input_top < 1) {
            lt->input_top = 1;
        }
        lt->cmd_top = lt->input_bottom + 1;

        /* chat-viewport: alles ueber der input-box. die zeilen-tabelle
         * wird jeden frame neu berechnet (resize veraendert die
         * breite und damit die umbrueche), die arena behalten wir.
         * waehrend einer anfrage reserviert der indikator die letzte
         * viewport-zeile fuer sich. */
        lt->chat_h = lt->input_top - 1;
        lt->chat_lines = NULL;
        lt->chat_lines_len = 0;
        lt->chat_first = 0;
        lt->busy_row = 0;
        if (lt->chat_h > 0 && st->busy) {
            lt->busy_row = lt->input_top - 1;
        }
        int view_h = lt->chat_h - ((lt->busy_row > 0) ? 1 : 0);
        if (view_h < 0) {
            view_h = 0;
        }
        if (view_h > 0 && st->chat.len > 0) {
            int text_w = lt->main_w - MSG_PREFIX_W;
            if (text_w < 1) {
                text_w = 1;
            }
            size_t need =
                chat_wrap(&st->chat, text_w, g_chat_lines, g_chat_lines_cap);
            if (need > g_chat_lines_cap) {
                size_t cap = (g_chat_lines_cap > 0) ? g_chat_lines_cap : 64;
                while (cap < need) {
                    if (cap > SIZE_MAX / 2) {
                        die("out of memory");
                    }
                    cap *= 2;
                }
                ChatLine *grown = realloc(g_chat_lines, cap * sizeof *grown);
                if (grown == NULL) {
                    die("out of memory");
                }
                g_chat_lines = grown;
                g_chat_lines_cap = cap;
                (void)chat_wrap(&st->chat, text_w, g_chat_lines,
                                g_chat_lines_cap);
            }
            if (need > (size_t)INT_MAX) {
                need = (size_t)INT_MAX; /* int-arithmetik unten */
            }
            lt->chat_lines = g_chat_lines;
            lt->chat_lines_len = need;

            /* scroll normalisieren und zurueckschreiben, wie es
             * layout_dialog_box mit DialogState macht: chat_scroll
             * zaehlt die zeilen, die UNTEN abgeschnitten sind, 0
             * heisst: dem ende folgen */
            int hidden = st->chat_scroll;
            int max_hidden = (int)need - view_h;
            if (hidden > max_hidden) {
                hidden = max_hidden;
            }
            if (hidden < 0) {
                hidden = 0;
            }
            st->chat_scroll = hidden;
            lt->chat_first = (int)need - hidden - view_h;
            if (lt->chat_first < 0) {
                lt->chat_first = 0; /* verlauf kuerzer als viewport: */
            } /* oben anfangen, rest bleibt leer */
        }
        break;

    case MODE_MODELS: {
        snprintf(lt->search, sizeof lt->search, "%s", st->dialog.search);
        lt->match_count = models_match(cfg, st->dialog.search, lt->match_idx,
                                       DIALOG_MATCH_MAX);

        /* id-spalte: laengste getroffene id (einmal pro frame, nicht
         * pro zeile) */
        lt->id_col = 0;
        for (int i = 0; i < lt->match_count; i++) {
            const char *url = "";
            const Model *m = model_at(cfg, lt->match_idx[i], &url);
            if (m != NULL && m->id != NULL) {
                int w = (int)strlen(m->id);
                if (w > lt->id_col) {
                    lt->id_col = w;
                }
            }
        }
        layout_dialog_box(lt, rows, &st->dialog);
        break;
    }

    case MODE_SETTINGS: {
        snprintf(lt->search, sizeof lt->search, "%s", st->dialog.search);
        lt->match_count =
            names_match(SETTING_NAMES, SET_COUNT, st->dialog.search,
                        lt->match_idx, DIALOG_MATCH_MAX);
        lt->id_col = names_col(SETTING_NAMES, SET_COUNT);
        layout_dialog_box(lt, rows, &st->dialog);
        break;
    }

    case MODE_THEME: {
        /* {NULL}: gcc sieht die fuellung in theme_names() nicht */
        const char *names[16] = {NULL};
        int total = theme_names(names, (int)(sizeof names / sizeof names[0]));
        snprintf(lt->search, sizeof lt->search, "%s", st->dialog.search);
        lt->match_count = names_match(names, total, st->dialog.search,
                                      lt->match_idx, DIALOG_MATCH_MAX);
        lt->id_col = names_col(names, total);
        layout_dialog_box(lt, rows, &st->dialog);
        break;
    }
    }
}

Slot layout_slot(const Layout *lt, int row)
{
    Slot s = {SLOT_BLANK, 0};

    /* die quit-meldung gewinnt immer, egal welcher modus aktiv ist */
    if (lt->quit_row != 0 && row == lt->quit_row) {
        s.kind = SLOT_QUIT;
        return s;
    }

    switch (lt->mode) {
    case MODE_INPUT:
        if (lt->busy_row > 0 && row == lt->busy_row) {
            s.kind = SLOT_BUSY;
            return s;
        }
        if (lt->chat_h > 0 && row < lt->input_top) {
            /* chat-verlauf ueber der input-box: jede viewport-zeile
             * ist eine render-zeile aus der tabelle */
            int line = lt->chat_first + (row - 1);
            if (line >= 0 && (size_t)line < lt->chat_lines_len) {
                s.index = line;
                switch (lt->chat_lines[line].role) {
                case CHAT_ROLE_USER:
                    s.kind = SLOT_MSG_USER;
                    break;
                case CHAT_ROLE_ASSISTANT:
                    s.kind = SLOT_MSG_ASSISTANT;
                    break;
                case CHAT_ROLE_ERROR:
                    s.kind = SLOT_MSG_ERROR;
                    break;
                case CHAT_ROLE_SYSTEM:
                case CHAT_ROLE_NOTICE:
                    s.kind = SLOT_MSG_SYSTEM;
                    break;
                case CHAT_ROLE_TOOL:
                    s.kind = SLOT_MSG_TOOL;
                    break;
                }
            }
            return s;
        }
        if (row == lt->input_top || row == lt->input_bottom) {
            s.kind = SLOT_BORDER;
        } else if (row > lt->input_top && row < lt->input_bottom) {
            s.kind = SLOT_INPUT;
            s.index = row - lt->input_top - 1;
        } else if (lt->cmd_h > 0 && row >= lt->cmd_top && row < lt->rows) {
            int vis = row - lt->cmd_top;
            if (lt->match_count > 0 && vis < lt->match_count) {
                s.kind = SLOT_CMD;
                s.index = lt->match_idx[vis];
            } else if (lt->match_count == 0 && vis == 0) {
                s.kind = SLOT_CMD_EMPTY;
            }
        }
        break;

    /* alle dialoge haben dieselbe box-geometrie (border, suchzeile,
     * eintraege) – nur der eintrags-slot ist modus-abhaengig */
    case MODE_MODELS:
    case MODE_SETTINGS:
    case MODE_THEME:
        if (row == lt->box_top) {
            s.kind = SLOT_DLG_BORDER;
        } else if (row == lt->box_top + 1) {
            s.kind = SLOT_DLG_SEARCH;
        } else if (row > lt->box_top + 1 && row <= lt->box_bottom) {
            int vis = row - lt->box_top - 2; /* 0-basierter sichtindex */
            if (lt->match_count == 0) {
                if (vis == 0) {
                    s.kind = SLOT_DLG_EMPTY; /* hinweis-zeile */
                }
            } else if (vis < lt->entries_h) {
                s.index = lt->match_idx[lt->scroll + vis]; /* scroll */
                switch (lt->mode) {
                case MODE_MODELS:
                    s.kind = SLOT_MODEL;
                    break;
                case MODE_SETTINGS:
                    s.kind = SLOT_SETTING;
                    break;
                case MODE_THEME:
                    s.kind = SLOT_THEME;
                    break;
                default:
                    break;
                }
            }
        }
        break;
    }
    return s;
}

/* einzelnes ascii-zeichen: genau eine sichtbare zelle */
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

/* zeilenrest mit leerzeichen auffuellen */
static void row_pad(Row *r)
{
    while (r->cells < r->width) {
        row_putc(r, ' ');
    }
}

static Row row_begin(Frame *f, int width)
{
    Row r = {f, width, 0};
    return r;
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

/* ganzer text */
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

static void row_input(Row *r, const Input *in, int idx)
{
    if (idx < 0 || idx >= (int)in->count) {
        return; /* feld groesser als der text: leerzeile */
    }
    row_puts(r, (idx == 0) ? " > " : "   ");

    const char *text = in->lines[idx];
    if (idx == (int)in->cursor_line) {
        /* cursor-block an der cursor-position (in der cursor-zeile).
         * klemmen ist defensively – die invariante gilt eigentlich
         * immer, aber der renderer liest hier text + cursor und darf
         * nie ueber das string-ende hinausschießen. */
        size_t cur = in->cursor;
        if (cur > strlen(text)) {
            cur = strlen(text);
        }
        row_putn(r, text, (int)cur);
        row_glyph(r, GLYPH_BLOCK);
        row_puts(r, text + cur);
    } else {
        row_puts(r, text);
    }
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

    /* alignment: id-spalte + 2 leerzeichen (wie cmd_name_col) */
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
    row_putn(r, name, slen); /* getippter anteil: suchfarbe */
    row_sgr(r, theme->reset);
    row_putn(r, name + slen, (int)strlen(name) - slen);

    /* alignment: name-spalte + 2 leerzeichen (wie cmd_name_col) */
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
    row_putn(r, name, slen); /* getippter anteil: suchfarbe */
    row_sgr(r, theme->reset);
    row_putn(r, name + slen, (int)strlen(name) - slen);

    /* alignment: name-spalte + 2 leerzeichen (wie cmd_name_col) */
    while (r->cells < 3 + id_col + 2) {
        row_putc(r, ' ');
    }
    if (active) {
        row_puts(r, "(active)");
    }
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

    /* alignment: name-spalte + 2 leerzeichen abstand zur beschreibung */
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

static void row_quit(Row *r)
{
    row_puts(r, "quit? ctrl+c again to confirm");
}

/* ------------------------------------------------------------------ */
/* chat-verlauf: label-zeile + eingerueckte folgezeilen. die farben  */
/* sind absichtlich feste SGRs (bold/rot) bzw. das theme-highlight   */
/* – eigene theme-felder dafuer kommen spaeter, wenn sich zeigt,   */
/* was gut aussieht.                                                */
/* ------------------------------------------------------------------ */
static void row_msg(Row *r, const ChatLine *ln, const char *text)
{
    const Theme *theme = theme_current();

    if (ln->first) {
        switch (ln->role) {
        case CHAT_ROLE_USER:
            row_sgr(r, "\x1b[1m"); /* bold */
            row_puts(r, "you  ");
            row_sgr(r, "\x1b[22m");
            break;
        case CHAT_ROLE_ASSISTANT:
            row_sgr(r, theme->match);
            row_puts(r, "max  ");
            row_sgr(r, theme->reset);
            break;
        case CHAT_ROLE_ERROR:
            row_sgr(r, "\x1b[31m"); /* rot */
            row_puts(r, "err  ");
            row_sgr(r, theme->reset);
            break;
        case CHAT_ROLE_TOOL:
            row_sgr(r, "\x1b[2m"); /* faint: maschinen-output */
            row_puts(r, "tool ");
            row_sgr(r, "\x1b[22m");
            break;
        case CHAT_ROLE_SYSTEM:
            row_puts(r, "sys  ");
            break;
        case CHAT_ROLE_NOTICE:
            row_sgr(r, "\x1b[2m"); /* faint: meldung der app, kein inhalt */
            row_puts(r, "ctx  ");
            row_sgr(r, "\x1b[22m");
            break;
        }
    } else {
        row_puts(r, "     "); /* unter MSG_PREFIX_W breit */
    }
    row_putn(r, text + ln->off, (int)ln->len);
}

/* darstellungs-zeile eines tool-calls: einrueckung, pfeil, name und
 * (abgeschnittene) argumente. laengere argumente (z.B. write_file-
 * inhalt) verschwinden am rand – der volle text steht in der api-
 * anfrage, nicht hier. */
static void row_tool_call(Row *r, const ChatToolCall *call)
{
    row_puts(r, "     ");
    row_puts(r, "\xE2\x86\x92 "); /* utf-8: rechts-pfeil */
    row_puts(r, (call->name != NULL) ? call->name : "?");
    row_puts(r, "(");
    row_puts(r, (call->arguments != NULL) ? call->arguments : "");
    row_puts(r, ")");
}

/* thinking-indikator waehrend einer laufenden anfrage: assistant-
 * label + abgedunkelter hinweis. die UI blockiert, bis die antwort
 * da ist – das hier ist die einzige sichtbare rueckmeldung. */
static void row_busy(Row *r)
{
    const Theme *theme = theme_current();
    row_sgr(r, theme->match);
    row_puts(r, "max  ");
    row_sgr(r, theme->reset);
    row_sgr(r, "\x1b[2m"); /* faint */
    row_puts(r, "thinking...");
    row_sgr(r, "\x1b[22m");
    row_glyph(r, GLYPH_BLOCK); /* cursor-block dahinter */
}

static const char *dlg_title(UIMode mode)
{
    switch (mode) {
    case MODE_MODELS:
        return "models";
    case MODE_SETTINGS:
        return "settings";
    case MODE_THEME:
        return "theme";
    default:
        return "";
    }
}

void draw_slot(Frame *f, const Layout *lt, const Slot *s, const AppState *st,
               const Config *cfg)
{
    Row r = row_begin(f, lt->main_w);

    switch (s->kind) {
    case SLOT_BLANK:
        break;
    case SLOT_BORDER:
        row_border(&r);
        break;
    case SLOT_INPUT:
        row_input(&r, &st->input, s->index);
        break;
    case SLOT_CMD_EMPTY:
        row_no_match(&r, lt->prefix);
        break;
    case SLOT_CMD:
        row_command(&r, &COMMANDS[s->index], lt->prefix);
        break;
    case SLOT_MSG_USER:
    case SLOT_MSG_ASSISTANT:
    case SLOT_MSG_ERROR:
    case SLOT_MSG_SYSTEM:
    case SLOT_MSG_TOOL: {
        const ChatLine *ln = &lt->chat_lines[s->index];
        if (ln->tool >= 0) {
            /* darstellungs-zeile eines tool-calls */
            row_tool_call(&r, &st->chat.msgs[ln->msg].tool_calls[ln->tool]);
            break;
        }
        row_msg(&r, ln, st->chat.msgs[ln->msg].text);
        break;
    }
    case SLOT_BUSY:
        row_busy(&r);
        break;
    case SLOT_DLG_BORDER:
        row_border(&r);
        break;
    case SLOT_DLG_SEARCH:
        row_dlg_search(&r, dlg_title(lt->mode), lt->search);
        break;
    case SLOT_DLG_EMPTY:
        row_no_dlg_match(&r, lt->search);
        break;
    case SLOT_MODEL:
        row_model(&r, cfg, s->index, lt->search, s->index == lt->selected,
                  lt->id_col);
        break;
    case SLOT_SETTING:
        row_setting(&r, cfg, s->index, lt->search, s->index == lt->selected,
                    lt->id_col);
        break;
    case SLOT_THEME: {
        const char *name = theme_option_name(s->index);
        bool active = false;
        if (name != NULL && strcmp(name, theme_current()->name) == 0) {
            active = true;
        }
        row_theme_opt(&r, s->index, lt->search, s->index == lt->selected,
                      active, lt->id_col);
        break;
    }
    case SLOT_QUIT:
        row_quit(&r);
        break;
    }

    row_pad(&r); /* jede zeile endet sauber am rand */
}

void draw(int rows, int cols, AppState *state, const DebugState *dbg,
          const Config *cfg)
{
    Frame f;
    frame_begin(&f, rows, cols);

    Layout lt;
    layout_compute(&lt, rows, cols, ui_mode(state), state, cfg);

#ifndef NDEBUG
    int dw = dbg_width(cols);
#else
    (void)dbg; /* release: sidebar ist kompiliert weg */
#endif

    for (int r = 1; r <= rows; r++) {
        Slot s = layout_slot(&lt, r);
        draw_slot(&f, &lt, &s, state, cfg);
#ifndef NDEBUG
        if (dw > 0) {
            f.pos += draw_sidebar(dbg, f.buf, f.pos, r, rows, dw);
        }
#endif
        frame_row_end(&f);
    }

    frame_end(&f, rows);
}
