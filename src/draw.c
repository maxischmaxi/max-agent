#define _POSIX_C_SOURCE 200809L // NOLINT(bugprone-reserved-identifier)

#include "draw.h"

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "chat.h"
#include "command.h"
#include "send.h"
#include "session.h"
#include "settings.h"
#include "state.h"
#include "theme.h"
#include "utils.h"

static const char GLYPH_EM_DASH[] = "\xE2\x80\x94";
static const char GLYPH_BLOCK[] = "\xE2\x96\x88";
static const char GLYPH_ARROW_UP[] = "\xE2\x86\x91";   /* utf-8: hoch */
static const char GLYPH_ARROW_DOWN[] = "\xE2\x86\x93"; /* utf-8: runter */

/* breite des zeilen-labels ("you  ", "max  ", ...) bzw. der
 * einrueckung von folgezeilen */
#define MSG_PREFIX_W 5

/* " > " bzw. "   " vor jeder eingabezeile */
#define INPUT_PREFIX_W 3

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

/* cursor/scroll normalisieren: NUR hier sind trefferzahl und
 * sichtbare hoehe gemeinsam bekannt -> zurueckschreiben in den
 * state, damit die key-behandlung (enter) konsistente werte
 * sieht, egal ob ein draw dazwischen lag */
static void dialog_normalize(Layout *lt, DialogState *d, int entries_h)
{
    lt->entries_h = entries_h;
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
}

void layout_dialog_box(Layout *lt, int rows, DialogState *d)
{
    /* eintrags-bereich: max 50% des bildschirms,
     * mind. 1 zeile (kein treffer: hinweis-zeile) */
    int entries_h = (lt->match_count > 0) ? lt->match_count : 1;
    if (entries_h > rows / 2) {
        entries_h = rows / 2;
    }
    if (entries_h < 1) {
        entries_h = 1;
    }
    dialog_normalize(lt, d, entries_h);

    /* box von unten: border, suchzeile, dann die eintraege */
    lt->box_top = rows - lt->entries_h - 2;
    lt->box_bottom = rows - 1;
}

/* full-screen-variante fuer den resume-dialog: die suchzeile sitzt
 * ganz oben (zeile 1), die eintraege fuellen den rest. box_top
 * ist 0 – eine zeile, die es nicht gibt –, dadurch zeichnet der
 * rahmen-slot nie, und die suchzeile rueckt auf zeile 1. die
 * allerletzte zeile bleibt wie ueberall frei: dort parkt der
 * cursor (frame_end), und die unterste rechte ecke zu beschreiben
 * kann aeltere terminals zum scrollen bringen. eingabefeld und
 * statuszeilen existieren in diesem modus nicht. */
static void layout_dialog_full(Layout *lt, int rows, DialogState *d)
{
    int entries_h = (lt->match_count > 0) ? lt->match_count : 1;
    if (entries_h > rows - 2) {
        entries_h = rows - 2;
    }
    if (entries_h < 1) {
        entries_h = 1;
    }
    dialog_normalize(lt, d, entries_h);

    lt->box_top = 0; /* "rahmen" im nirgendwo: nur suche + eintraege */
    lt->box_bottom = rows - 1;
}

int input_field_width(int cols)
{
    /* eine spalte mehr als umgebrochen wird: dort sitzt der
     * cursor-block, wenn er am zeilenende steht */
    int w = main_width(cols) - INPUT_PREFIX_W - 1;
    return (w > 0) ? w : 1;
}

void layout_compute(Layout *lt, int rows, int cols, UIMode mode, AppState *st,
                    const Config *cfg)
{
    memset(lt, 0, sizeof *lt);
    lt->mode = mode;
    lt->rows = rows;
    lt->main_w = main_width(cols);

    /* die zwei statuszeilen ganz unten gehoeren niemandem sonst;
     * zeile `rows` bleibt frei, dort parkt frame_end den cursor.
     *
     * sie bekommen den platz aber nur, wenn darueber noch eine
     * eingabe-box passt (rahmen, textzeile, rahmen) – sonst wuerde
     * bottom_border_for auf sein minimum klemmen und die box liefe
     * in die statuszeilen hinein. */
    int usable = rows;
    lt->status_row = 0;
    if (rows >= STATUS_H + 3) {
        lt->status_row = rows - STATUS_H;
        usable = rows - STATUS_H;
    }

    if (st->confirm_quit) {
        lt->quit_row = usable - 1;
    }

    switch (mode) {
    case MODE_INPUT:
        if (st->cmd_active) {
            cmd_prefix(&st->input, lt->prefix, sizeof lt->prefix);
            lt->match_count =
                cmd_match(lt->prefix, lt->match_idx, COMMAND_COUNT);
            /* kein treffer: eine zeile fuer den hinweis reservieren */
            lt->cmd_h = (lt->match_count > 0) ? lt->match_count : 1;
        } else if (st->prompt_edit) {
            /* dieselbe zeile wie die befehlsliste, nur mit dem
             * hinweis, dass enter hier den prompt speichert */
            lt->cmd_h = 1;
            lt->prompt_hint = true;
        }
        lt->input_bottom =
            bottom_border_for(usable, lt->cmd_h, st->confirm_quit);

        /* die hoehe kommt aus den BILDSCHIRMzeilen, nicht aus der
         * zahl der logischen zeilen: eine lange zeile bricht um und
         * braucht dann mehrere. bei einem resize faellt das hier neu
         * aus, ohne dass der text angefasst wird. */
        lt->input_w = input_field_width(cols);
        size_t in_rows = input_screen_rows(&st->input, lt->input_w);

        /* die box darf den verlauf nicht ganz verdraengen: zwei
         * zeilen bleiben oben frei (dieselbe regel, nach der
         * input_newline entscheidet). passt der text nicht, wird
         * gescrollt statt gewachsen. */
        int max_h = lt->input_bottom - 3;
        if (max_h < 1) {
            max_h = 1;
        }
        int in_h = (in_rows > (size_t)max_h) ? max_h : (int)in_rows;
        if (in_h < 1) {
            in_h = 1;
        }
        lt->input_top = lt->input_bottom - in_h - 1;
        if (lt->input_top < 1) {
            lt->input_top = 1;
        }

        /* mitscrollen, damit die zeile mit dem cursor sichtbar ist */
        lt->input_first = 0;
        if (in_rows > (size_t)in_h) {
            size_t crow = 0;
            input_cursor_screen(&st->input, lt->input_w, &crow, NULL);
            if (crow >= (size_t)in_h) {
                lt->input_first = crow - (size_t)in_h + 1;
            }
            if (lt->input_first + (size_t)in_h > in_rows) {
                lt->input_first = in_rows - (size_t)in_h;
            }
        }
        lt->cmd_top = lt->input_bottom + 1;

        /* chat-viewport: alles ueber der input-box. die zeilen-tabelle
         * wird jeden frame neu berechnet (resize veraendert die
         * breite und damit die umbrueche), die arena behalten wir.
         * waehrend einer anfrage reserviert der indikator die letzte
         * viewport-zeile fuer sich. */
        lt->chat_h = lt->input_top - 1;
        lt->chat_top = 1;
        lt->chat_lines = NULL;
        lt->chat_lines_len = 0;
        lt->chat_first = 0;
        lt->busy_row = 0;
        lt->more_above = 0;
        lt->more_below = 0;
        lt->more_above_row = 0;
        lt->more_below_row = 0;
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
             * heisst: dem ende folgen.
             *
             * die beiden scroll-hinweise kosten je eine zeile, und ob
             * sie gebraucht werden, haengt am scroll-stand – der
             * wiederum am verbleibenden platz. zweimal rechnen
             * genuegt: ein eingeblendeter hinweis verdeckt nur mehr
             * zeilen, nie weniger, die flags kippen also nicht
             * zurueck. */
            int above = 0;
            int below = 0;
            int hidden = 0;
            for (int pass = 0; pass < 2; pass++) {
                int hints = (above > 0 ? 1 : 0) + (below > 0 ? 1 : 0);
                if (hints > view_h - 1) {
                    hints = 0; /* zu eng: verlauf schlaegt hinweis */
                }
                int h = view_h - hints;

                hidden = st->chat_scroll;
                int max_hidden = (int)need - h;
                if (hidden > max_hidden) {
                    hidden = max_hidden;
                }
                if (hidden < 0) {
                    hidden = 0;
                }
                int first = (int)need - hidden - h;
                if (first < 0) {
                    first = 0; /* verlauf kuerzer als viewport: */
                } /* oben anfangen, rest bleibt leer */
                above = first;
                below = hidden;
                lt->chat_first = first;
            }
            st->chat_scroll = hidden;

            /* die hinweise an ihre zeilen setzen – aber nur, wenn
             * danach ueberhaupt verlauf uebrig bleibt */
            int hints = (above > 0 ? 1 : 0) + (below > 0 ? 1 : 0);
            if (hints <= view_h - 1) {
                lt->more_above = above;
                lt->more_below = below;
                if (above > 0) {
                    lt->more_above_row = 1;
                    lt->chat_top = 2;
                }
                if (below > 0) {
                    /* unter dem verlauf, aber ueber dem thinking-
                     * indikator, falls der gerade laeuft */
                    lt->more_below_row = (lt->busy_row > 0) ? lt->busy_row - 1
                                                            : lt->input_top - 1;
                }
            }
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
        layout_dialog_box(lt, usable, &st->dialog);
        break;
    }

    case MODE_SETTINGS: {
        snprintf(lt->search, sizeof lt->search, "%s", st->dialog.search);
        lt->match_count =
            names_match(SETTING_NAMES, SET_COUNT, st->dialog.search,
                        lt->match_idx, DIALOG_MATCH_MAX);
        lt->id_col = names_col(SETTING_NAMES, SET_COUNT);
        layout_dialog_box(lt, usable, &st->dialog);
        break;
    }

    case MODE_PROMPT:
        snprintf(lt->search, sizeof lt->search, "%s", st->dialog.search);
        lt->match_count =
            names_match(PROMPT_OPT_NAMES, PROMPT_OPT_COUNT, st->dialog.search,
                        lt->match_idx, DIALOG_MATCH_MAX);
        lt->id_col = names_col(PROMPT_OPT_NAMES, PROMPT_OPT_COUNT);
        layout_dialog_box(lt, usable, &st->dialog);
        break;

    case MODE_THEME: {
        /* {NULL}: gcc sieht die fuellung in theme_names() nicht */
        const char *names[16] = {NULL};
        int total = theme_names(names, (int)(sizeof names / sizeof names[0]));
        snprintf(lt->search, sizeof lt->search, "%s", st->dialog.search);
        lt->match_count = names_match(names, total, st->dialog.search,
                                      lt->match_idx, DIALOG_MATCH_MAX);
        lt->id_col = names_col(names, total);
        layout_dialog_box(lt, usable, &st->dialog);
        break;
    }

    case MODE_SESSIONS: {
        /* full screen: die statuszeilen gehoeren in diesem modus
         * dem dialog, niemand sonst wertet sie hier aus */
        lt->status_row = 0;
        snprintf(lt->search, sizeof lt->search, "%s", st->dialog.search);
        lt->match_count = sessions_match(&st->sessions, st->dialog.search,
                                         lt->match_idx, DIALOG_MATCH_MAX);

        /* spaltenbreiten: id fest, name/preview gedeckelt. die
         * name-spalte bekommt nur, was nach id, datum und nach-
         * richten-zahl uebrig bleibt – sonst drueckt eine lange
         * erste nachricht die zeit-spalte vom rand. die rest-
         * breite: (3 " > " + 2 abstand + id + 2 abstand + 11
         * datum + 5 trenner + 16 "9999 nachrichten") */
        lt->id_col = 0;
        for (int i = 0; i < lt->match_count; i++) {
            int idw = (int)strlen(st->sessions.items[lt->match_idx[i]].id);
            if (idw > lt->id_col) {
                lt->id_col = idw;
            }
        }
        int free_w = lt->main_w - lt->id_col - 39;
        if (free_w < 1) {
            free_w = 1; /* sehr schmal: name-spalte kollabiert */
        }
        lt->name_col = 0;
        for (int i = 0; i < lt->match_count; i++) {
            const SessionInfo *si = &st->sessions.items[lt->match_idx[i]];
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
            if (nw > lt->name_col) {
                lt->name_col = nw;
            }
        }
        layout_dialog_full(lt, rows, &st->dialog);
        break;
    }
    }
}

Slot layout_slot(const Layout *lt, int row)
{
    Slot s = {SLOT_BLANK, 0};

    /* die quit-meldung gewinnt immer, egal welcher modus aktiv ist */
    if (lt->status_row > 0 && row >= lt->status_row &&
        row < lt->status_row + STATUS_H) {
        s.kind =
            (row == lt->status_row) ? SLOT_STATUS_MODEL : SLOT_STATUS_TOKENS;
        return s;
    }
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
        if (lt->more_above_row > 0 && row == lt->more_above_row) {
            s.kind = SLOT_MORE_ABOVE;
            return s;
        }
        if (lt->more_below_row > 0 && row == lt->more_below_row) {
            s.kind = SLOT_MORE_BELOW;
            return s;
        }
        if (lt->chat_h > 0 && row >= lt->chat_top && row < lt->input_top) {
            /* chat-verlauf ueber der input-box: jede viewport-zeile
             * ist eine render-zeile aus der tabelle */
            int line = lt->chat_first + (row - lt->chat_top);
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
            if (lt->prompt_hint) {
                if (vis == 0) {
                    s.kind = SLOT_PROMPT_HINT;
                }
            } else if (lt->match_count > 0 && vis < lt->match_count) {
                s.kind = SLOT_CMD;
                s.index = lt->match_idx[vis];
            } else if (lt->match_count == 0 && vis == 0) {
                s.kind = SLOT_CMD_EMPTY;
            }
        }
        break;

    /* alle dialoge haben dieselbe box-geometrie (border, suchzeile,
     * eintraege) – nur der eintrags-slot ist modus-abhaengig. der
     * resume-dialog nutzt dieselbe struktur, nur halt full screen
     * (layout_dialog_full setzt box_top auf 0: der rahmen-slot
     * zeichnet dann nie, die suchzeile sitzt auf zeile 1) */
    case MODE_MODELS:
    case MODE_SETTINGS:
    case MODE_PROMPT:
    case MODE_THEME:
    case MODE_SESSIONS:
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
                case MODE_PROMPT:
                    s.kind = SLOT_PROMPT_OPT;
                    break;
                case MODE_THEME:
                    s.kind = SLOT_THEME;
                    break;
                case MODE_SESSIONS:
                    s.kind = SLOT_SESSION;
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
    /* das prompt-zeichen gehoert an den textanfang, nicht an jede
     * umgebrochene zeile */
    row_puts(r, (idx == 0) ? " > " : "   ");

    const char *text = in->lines[line] + off;

    size_t crow = 0;
    input_cursor_screen(in, width, &crow, NULL);
    if (crow != idx) {
        row_putn(r, text, (int)len);
        return;
    }

    /* der cursor steht in dieser zeile. sein byte-offset im
     * abschnitt ist die differenz zum abschnittsanfang – klemmen
     * ist defensively, die invariante gilt eigentlich immer. */
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
    case SET_SYSTEM_PROMPT:
        /* die drei config-zustaende auf einen blick */
        if (cfg->system_prompt == NULL) {
            row_puts(r, "default");
        } else if (cfg->system_prompt[0] == '\0') {
            row_puts(r, "off");
        } else {
            /* eigener text: der rest wird am rand abgeschnitten */
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

/* eintrag des system-prompt-untermenues. "(active)" haengt an der
 * option, die dem aktuellen config-zustand entspricht – "edit" ist
 * eine aktion und nie aktiv. */
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
    row_putn(r, name, slen); /* getippter anteil: suchfarbe */
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
 * bearbeitet: ohne ihn waere nicht zu erkennen, dass enter hier
 * speichert statt zu senden. */
static void row_prompt_hint(Row *r)
{
    row_sgr(r, theme_role(THEME_ROLE_DIM));
    row_puts(r, "  system prompt: enter speichert, esc verwirft");
    row_sgr(r, THEME_ROLE_RESET);
}

/* unix-ms -> "dd.mm hh:mm" (bzw. mit jahr, wenn es aelter ist).
 * die liste soll auf einen blick zeigen, wann eine session zuletzt
 * aktiv war – keine sekunden-genauigkeit noetig. */
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

/* eintrag der session-liste (resume-dialog). erste spalte: name,
 * sonst die gekuerzte erste nachricht, sonst ein hinweis. danach
 * id, zeitstempel und nachrichten-zahl; die gerade offene session
 * traegt "(aktiv)". */
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
    /* name auf die spaltenbreite kappen: die tabelle bleibt auch
     * auf schmalen terminals lesbar, id/datum/nachrichten-zahl
     * rutschen nie vom rand */
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

    /* alignment: name-spalte (+ marker der aktiven session) + 2.
     * die spaltenbreite einmal berechnen und fuer beide padding-
     * schleifen benutzen. */
    int col = 3 + name_col + 2;
    if (active) {
        col += 8;
    }
    while (r->cells < col) {
        row_putc(r, ' ');
    }

    row_sgr(r, theme_role(THEME_ROLE_DIM));
    row_putn(r, si->id, slen); /* id matcht die suche mit */
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

/* token-zahlen kurz halten: 1234 -> "1.2k", 45678 -> "45k".
 * die statuszeile soll auf einen blick lesbar sein, nicht exakt. */
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

/* label einer statuszeile: gleiche breite wie die chat-labels,
 * damit die spalten untereinander stehen */
static void status_label(Row *r, const char *label)
{
    row_sgr(r, theme_role(THEME_ROLE_DIM));
    row_puts(r, " ");
    row_puts(r, label);
    row_sgr(r, THEME_ROLE_RESET);
}

/* statuszeile 1: mit wem wir sprechen und wie gross dessen fenster
 * ist. ohne gewaehltes modell ein hinweis, wie man eins waehlt. */
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
        row_puts(r, "  \xC2\xB7  "); /* mittelpunkt */
        put_count(r, m->context_window);
        row_puts(r, " kontext");
    }
    if (provider != NULL && provider->base_url != NULL) {
        row_puts(r, "  \xC2\xB7  ");
        row_puts(r, provider->base_url);
    }
    row_sgr(r, THEME_ROLE_RESET);
}

/* statuszeile 2: was die sitzung bisher gekostet hat, rechts
 * daneben name und id der offenen session (der platz dafuer ist
 * hier reserviert). ohne aktive session bleibt es bei den
 * tokens. */
static void row_status_tokens(Row *r, const AppState *st)
{
    status_label(r, "tokens  ");

    row_sgr(r, theme_role(THEME_ROLE_DIM));
    put_count(r, st->ctx.total_prompt);
    row_puts(r, " gesendet  \xC2\xB7  ");
    put_count(r, st->ctx.total_completion);
    row_puts(r, " empfangen");

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
    row_sgr(r, THEME_ROLE_RESET);
}

static void row_quit(Row *r)
{
    row_puts(r, "quit? ctrl+c again to confirm");
}

/* ------------------------------------------------------------------ */
/* chat-verlauf: label-zeile + eingerueckte folgezeilen. die farben  */
/* kommen aus dem theme (theme_role), nicht aus festen sequenzen –   */
/* ein theme kann fuer jede rolle farbe oder attribut waehlen.       */
/* ------------------------------------------------------------------ */
/* label und farbe einer rolle: beides gehoert zusammen, damit eine
 * neue rolle nur hier eingetragen werden muss */
static ThemeRole role_theme(ChatRole role, const char **label)
{
    switch (role) {
    case CHAT_ROLE_USER:
        *label = "you  ";
        return THEME_ROLE_USER;
    case CHAT_ROLE_ASSISTANT:
        *label = "max  ";
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
    row_sgr(r, theme_role(THEME_ROLE_TOOL));
    row_puts(r, "\xE2\x86\x92 "); /* utf-8: rechts-pfeil */
    row_puts(r, (call->name != NULL) ? call->name : "?");
    row_puts(r, "(");
    row_puts(r, (call->arguments != NULL) ? call->arguments : "");
    row_puts(r, ")");
    row_sgr(r, THEME_ROLE_RESET);
}

/* thinking-indikator waehrend einer laufenden anfrage: assistant-
 * label + abgedunkelter hinweis. die UI blockiert, bis die antwort
 * da ist – das hier ist die einzige sichtbare rueckmeldung. */
/* scroll-hinweis am rand des viewports: pfeil, anzahl, faint.
 * die zahl zaehlt render-zeilen, nicht nachrichten – das ist die
 * groesse, in der der viewport rechnet. */
static void row_more(Row *r, const char *arrow, int lines)
{
    char buf[48];
    (void)snprintf(buf, sizeof buf, " %d weitere %s", lines,
                   (lines == 1) ? "zeile" : "zeilen");
    row_puts(r, "     "); /* auf MSG_PREFIX_W einruecken */
    row_sgr(r, theme_role(THEME_ROLE_DIM));
    row_glyph(r, arrow);
    row_puts(r, buf);
    row_sgr(r, THEME_ROLE_RESET);
}

static void row_busy(Row *r)
{
    row_sgr(r, theme_role(THEME_ROLE_ASSISTANT));
    row_puts(r, "max  ");
    row_sgr(r, THEME_ROLE_RESET);
    row_sgr(r, theme_role(THEME_ROLE_DIM));
    row_puts(r, "thinking...");
    row_sgr(r, THEME_ROLE_RESET);
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
    case MODE_SESSIONS:
        return "resume";
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
        row_input(&r, &st->input, lt->input_w,
                  lt->input_first + (size_t)s->index);
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
    case SLOT_MORE_ABOVE:
        row_more(&r, GLYPH_ARROW_UP, lt->more_above);
        break;
    case SLOT_MORE_BELOW:
        row_more(&r, GLYPH_ARROW_DOWN, lt->more_below);
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
    case SLOT_PROMPT_OPT: {
        /* aktiv ist die option, die zum config-zustand passt */
        bool active = false;
        if (s->index == PROMPT_DEFAULT) {
            active = (cfg->system_prompt == NULL);
        } else if (s->index == PROMPT_OFF && cfg->system_prompt != NULL &&
                   cfg->system_prompt[0] == '\0') {
            active = true;
        }
        row_prompt_opt(&r, s->index, lt->search, s->index == lt->selected,
                       active, lt->id_col);
        break;
    }
    case SLOT_SESSION: {
        const SessionInfo *si = &st->sessions.items[s->index];
        bool active = false;
        if (st->session.active && strcmp(st->session.id, si->id) == 0) {
            active = true;
        }
        row_session(&r, si, lt->search, s->index == lt->selected, active,
                    lt->id_col, lt->name_col);
        break;
    }
    case SLOT_PROMPT_HINT:
        row_prompt_hint(&r);
        break;
    case SLOT_QUIT:
        row_quit(&r);
        break;
    case SLOT_STATUS_MODEL:
        row_status_model(&r, cfg);
        break;
    case SLOT_STATUS_TOKENS:
        row_status_tokens(&r, st);
        break;
    }

    row_pad(&r); /* jede zeile endet sauber am rand */
}

void draw(int rows, int cols, AppState *state, const Config *cfg)
{
    Frame f;
    frame_begin(&f, rows, cols);

    Layout lt;
    layout_compute(&lt, rows, cols, ui_mode(state), state, cfg);

    for (int r = 1; r <= rows; r++) {
        Slot s = layout_slot(&lt, r);
        draw_slot(&f, &lt, &s, state, cfg);
        frame_row_end(&f);
    }

    frame_end(&f, rows);
}
