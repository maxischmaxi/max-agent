#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <unistd.h>

#include "command.h"
#include "config.h"
#include "debug.h"
#include "input.h"
#include "keys.h"
#include "theme.h"
#include "utils.h"

typedef struct {
    bool confirm_quit;  /* when ctrl+c was hit the first time */
    bool models_dialog; /* when entering the models dialog */
    bool cmd_active;    /* when typing "/", currently writing a command */
    bool settings_dialog;
} AppState;

/* ------------------------------------------------------------------ */
/* UI-Modi: der modus entscheidet, welche bereiche das layout auf dem */
/* bildschirm platziert. ein neuer dialog ist genau ein neuer MODE_* */
/* plus ein case in layout_compute() und layout_slot() – mehr nicht.  */
/* ------------------------------------------------------------------ */
typedef enum {
    MODE_INPUT,    /* eingabefeld + optionale befehlsliste */
    MODE_MODELS,   /* modell-liste am unteren rand statt eingabefeld */
    MODE_SETTINGS, /* settings-dialog (noch ein stub) */
} UIMode;

static UIMode ui_mode(const AppState *st)
{
    if (st->models_dialog) {
        return MODE_MODELS;
    }
    if (st->settings_dialog) {
        return MODE_SETTINGS;
    }
    return MODE_INPUT;
}

/* breite des hauptbereichs (ohne separator + sidebar) */
static int main_width(int cols)
{
    int dw = dbg_width(cols);
    return (dw > 0) ? (cols - dw - 1) : cols;
}
static volatile sig_atomic_t g_resized = 0;

static void on_winch(int sig)
{
    (void)sig;
    g_resized = 1;
}

static size_t setup_pos(char *buf)
{
    size_t res = 0;
    /* cursor home + attribute-reset: jeder frame beginnt im
     * definierten zustand, farben koennen nie ueber frames leaken
     * (ansi-attribute sind im terminal zustandsbehaftet). */
    res += (size_t)sprintf(buf + res, "\x1b[H\x1b[m");
    return res;
}

/* ================================================================== */
/* Frame: kompletter bildschirminhalt wird in einen puffer geschrie-  */
/* ben und am ende mit einem einzigen fwrite ausgegeben.             */
/* ================================================================== */

typedef struct {
    char *buf;
    size_t pos;
} Frame;

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

/* ================================================================== */
/* Row: schreibt genau EINE zeile des hauptbereichs. zaehlt sichtbare */
/* zellen mit, damit ansi-codes die breitenrechnung nicht stoeren,    */
/* clippt am rechten rand und fuellt am schluss mit leerzeichen auf. */
/* renderer muessen sich um nichts davon kuemmern.                   */
/* ================================================================== */

typedef struct {
    Frame *f;
    int width; /* zellen bis zum rand des hauptbereichs */
    int cells; /* bereits geschriebene sichtbare zellen */
} Row;

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

/* einzelnes ascii-zeichen: genau eine sichtbare zelle */
static void row_putc(Row *r, char c)
{
    if (r->cells >= r->width) {
        return;
    }
    r->f->buf[r->f->pos++] = c;
    r->cells++;
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

/* utf-8-symbol, das genau eine terminal-zelle breit ist */
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

static const char GLYPH_EM_DASH[] = "\xE2\x80\x94";
static const char GLYPH_BLOCK[] = "\xE2\x96\x88";

/* ================================================================== */
/* Row-Renderer: jeder renderer zeichnet den inhalt einer zeile fuer */
/* einen layout-bereich. padding und clipping macht die Row-API.     */
/* ================================================================== */

static void row_border(Row *r)
{
    while (r->cells < r->width) {
        row_glyph(r, GLYPH_EM_DASH);
    }
}

/* eine zeile des eingabefeldes: prompt ab zeile 0, cursor-block am
 * ende der letzten text-zeile */
static void row_input(Row *r, const Input *in, int idx)
{
    if (idx < 0 || idx >= (int)in->count) {
        return; /* feld groesser als der text: leerzeile */
    }
    row_puts(r, (idx == 0) ? " > " : "   ");
    row_puts(r, in->lines[idx]);
    if (idx == (int)in->count - 1) {
        row_glyph(r, GLYPH_BLOCK);
    }
}

/* eintrag der befehlsliste: der getippte prefix-anteil des namens
 * wird farblich hervorgehoben */
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

/* das i-te modell, ueber alle provider hinweg gezaehlt */
static const Model *model_at(const Config *cfg, int idx, const char **url)
{
    for (size_t p = 0; p < cfg->providers_len; p++) {
        const Provider *pr = &cfg->providers[p];
        if (idx < (int)pr->models_len) {
            *url = pr->base_url;
            return &pr->models[idx];
        }
        idx -= (int)pr->models_len;
    }
    return NULL;
}

static int models_total(const Config *cfg)
{
    int n = 0;
    for (size_t p = 0; p < cfg->providers_len; p++) {
        n += (int)cfg->providers[p].models_len;
    }
    return n;
}

static void row_models_title(Row *r, int total)
{
    char line[64];
    snprintf(line, sizeof line, " models: %d  (esc to close)", total);
    row_puts(r, line);
}

static void row_models_empty(Row *r)
{
    row_puts(r, "  no models configured");
}

static void row_model(Row *r, const Config *cfg, int idx)
{
    const char *url = "";
    const Model *m = model_at(cfg, idx, &url);
    if (m == NULL) {
        return;
    }
    row_puts(r, "  ");
    row_puts(r, (m->id != NULL) ? m->id : "?");
    row_puts(r, "   ");
    row_puts(r, (url != NULL) ? url : "");
    if (m->reasoning) {
        row_puts(r, "  (reasoning)");
    }
}

static void row_settings_title(Row *r)
{
    row_puts(r, " settings (not implemented yet)");
}

static void row_settings_hint(Row *r)
{
    row_puts(r, "  esc to close");
}

/* ================================================================== */
/* Layout: entscheidet pro bildschirm-zeile, WAS dort gezeichnet     */
/* wird. positionierung und rendering sind damit komplett entkoppelt.*/
/* ================================================================== */

typedef enum {
    SLOT_BLANK,          /* leere zeile */
    SLOT_BORDER,         /* trenn-linie aus em-dashes */
    SLOT_INPUT,          /* zeile des eingabefeldes */
    SLOT_CMD_EMPTY,      /* hinweis: kein befehl passt */
    SLOT_CMD,            /* treffer der befehlsliste */
    SLOT_MODELS_TITLE,   /* kopf der modell-liste */
    SLOT_MODELS_EMPTY,   /* hinweis: keine modelle */
    SLOT_MODEL,          /* eintrag der modell-liste */
    SLOT_SETTINGS_TITLE, /* kopf des settings-dialogs */
    SLOT_SETTINGS_HINT,  /* hinweis-zeile des settings-dialogs */
    SLOT_QUIT,           /* quit-bestaetigung */
} SlotKind;

typedef struct {
    SlotKind kind;
    int index; /* listen-index: eingabe-zeile / befehl / modell */
} Slot;

typedef struct {
    UIMode mode;
    int rows;
    int main_w;

    /* eingabefeld inkl. rahmen: erste/letzte zeile, 1-basiert */
    int input_top;
    int input_bottom;

    /* befehlsliste direkt unter dem eingabefeld */
    int cmd_top;
    int cmd_h;

    /* modell-liste am unteren rand (nur MODE_MODELS) */
    int models_top;
    int models_h;

    int quit_row; /* 0 = keine quit-meldung anzeigen */

    /* daten fuer die renderer */
    char prefix[64];
    int match_idx[COMMAND_COUNT];
    int match_count;
    int model_total;
} Layout;

static void layout_compute(Layout *lt, int rows, int cols, UIMode mode,
                           const Input *in, const AppState *st,
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
            cmd_prefix(in, lt->prefix, sizeof lt->prefix);
            lt->match_count =
                cmd_match(lt->prefix, lt->match_idx, COMMAND_COUNT);
            /* kein treffer: eine zeile fuer den hinweis reservieren */
            lt->cmd_h = (lt->match_count > 0) ? lt->match_count : 1;
        }
        lt->input_bottom = bottom_border_for(rows, lt->cmd_h, st->confirm_quit);
        lt->input_top = lt->input_bottom - (int)in->count - 1;
        if (lt->input_top < 1) {
            lt->input_top = 1;
        }
        lt->cmd_top = lt->input_bottom + 1;
        break;

    case MODE_MODELS:
        lt->model_total = models_total(cfg);
        lt->models_h = lt->model_total;
        /* kopf-zeile der liste braucht eine eigene zeile */
        if (lt->models_h > rows - 2) {
            lt->models_h = rows - 2;
        }
        if (lt->models_h < 0) {
            lt->models_h = 0;
        }
        /* leerer dialog: trotzdem eine zeile fuer den hinweis */
        lt->models_top = rows - ((lt->models_h > 0) ? lt->models_h : 1);
        break;

    case MODE_SETTINGS:
        break; /* zwei fixe zeilen unten, siehe layout_slot() */
    }
}

/* die zentrale tabelle: zeilen-nummer -> was gehoert dort hin */
static Slot layout_slot(const Layout *lt, int row)
{
    Slot s = {SLOT_BLANK, 0};

    /* die quit-meldung gewinnt immer, egal welcher modus aktiv ist */
    if (lt->quit_row != 0 && row == lt->quit_row) {
        s.kind = SLOT_QUIT;
        return s;
    }

    switch (lt->mode) {
    case MODE_INPUT:
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

    case MODE_MODELS:
        if (row == lt->models_top - 1) {
            s.kind = SLOT_MODELS_TITLE;
        } else if (row == lt->models_top && lt->models_h == 0) {
            s.kind = SLOT_MODELS_EMPTY;
        } else if (row >= lt->models_top &&
                   row - lt->models_top < lt->models_h) {
            s.kind = SLOT_MODEL;
            s.index = row - lt->models_top;
        }
        break;

    case MODE_SETTINGS:
        if (row == lt->rows - 1) {
            s.kind = SLOT_SETTINGS_HINT;
        } else if (row == lt->rows - 2) {
            s.kind = SLOT_SETTINGS_TITLE;
        }
        break;
    }
    return s;
}

/* rendert genau einen slot als eine zeile des hauptbereichs */
static void draw_slot(Frame *f, const Layout *lt, const Slot *s,
                      const Input *in, const Config *cfg)
{
    Row r = row_begin(f, lt->main_w);

    switch (s->kind) {
    case SLOT_BLANK:
        break;
    case SLOT_BORDER:
        row_border(&r);
        break;
    case SLOT_INPUT:
        row_input(&r, in, s->index);
        break;
    case SLOT_CMD_EMPTY:
        row_no_match(&r, lt->prefix);
        break;
    case SLOT_CMD:
        row_command(&r, &COMMANDS[s->index], lt->prefix);
        break;
    case SLOT_MODELS_TITLE:
        row_models_title(&r, lt->model_total);
        break;
    case SLOT_MODELS_EMPTY:
        row_models_empty(&r);
        break;
    case SLOT_MODEL:
        row_model(&r, cfg, s->index);
        break;
    case SLOT_SETTINGS_TITLE:
        row_settings_title(&r);
        break;
    case SLOT_SETTINGS_HINT:
        row_settings_hint(&r);
        break;
    case SLOT_QUIT:
        row_quit(&r);
        break;
    }

    row_pad(&r); /* jede zeile endet sauber am rand */
}

/* die draw-funktion selbst ist nur noch die pipeline:
 * layout berechnen -> jede zeile klassifizieren -> zeile rendern */
static void draw(int rows, int cols, const Input *in, const AppState *state,
                 const DebugState *dbg, const Config *cfg)
{
    Frame f;
    frame_begin(&f, rows, cols);

    Layout lt;
    layout_compute(&lt, rows, cols, ui_mode(state), in, state, cfg);

#ifndef NDEBUG
    int dw = dbg_width(cols);
#else
    (void)dbg; /* release: sidebar ist kompiliert weg */
#endif

    for (int r = 1; r <= rows; r++) {
        Slot s = layout_slot(&lt, r);
        draw_slot(&f, &lt, &s, in, cfg);
#ifndef NDEBUG
        if (dw > 0) {
            f.pos += draw_sidebar(dbg, f.buf, f.pos, r, rows, dw);
        }
#endif
        frame_row_end(&f);
    }

    frame_end(&f, rows);
}

/* hoehenreservation fuer die befehlsliste, damit input_newline weiss,
 * wie viel platz noch frei ist (layout_compute() nutzt dieselbe logik) */
static int cmd_list_height(const Input *in, bool cmd_active)
{
    if (!cmd_active) {
        return 0;
    }
    char prefix[64];
    int idx[COMMAND_COUNT];
    cmd_prefix(in, prefix, sizeof prefix);
    int n = cmd_match(prefix, idx, COMMAND_COUNT);
    return (n > 0) ? n : 1; /* kein treffer: hinweis-zeile reservieren */
}

static void cmd_parse(const Input *in, char *word, size_t word_sz, char *args,
                      size_t args_sz)
{
    word[0] = '\0';
    args[0] = '\0';

    const char *last = in->lines[in->count - 1];
    if (last[0] != '/') {
        return;
    }

    const char *sp = strchr(last, ' ');
    size_t wlen = (sp != NULL) ? (size_t)(sp - last) : strlen(last);
    if (wlen >= word_sz) {
        wlen = word_sz - 1;
    }
    memcpy(word, last, wlen);
    word[wlen] = '\0';

    if (sp != NULL && args_sz > 0) {
        sp++; /* leerzeichen ueberspringen */
        size_t alen = strlen(sp);
        if (alen >= args_sz) {
            alen = args_sz - 1;
        }
        memcpy(args, sp, alen);
        args[alen] = '\0';
    }
}

int main(void)
{
    /* debug-zustand gehoert main und wird als pointer durchgereicht.
     * static: ~82KB, zero-initialisiert, kein stack-verbrauch. */
    static DebugState dbg;

    atexit(restore);
    raw_enable();
    screen_enter();
    signal(SIGWINCH, on_winch);

    /* default-farben vom terminal abfragen (muss nach raw_enable
     * passieren, sonst koennen antworten nicht gelesen werden).
     * fruehe tastatur-eingaben landen im leftover und werden dem
     * keys-modul zurueckgegeben, damit nichts verloren geht. */
    char leftover[SEQ_MAX];
    size_t leftover_len = 0;
    theme_init(leftover, sizeof leftover, &leftover_len);
    if (leftover_len > 0) {
        keys_unread(leftover, leftover_len);
    }
    dbg_log(&dbg, "theme: %s (match=%s)", theme_current()->name,
            theme_current()->match);

    int rows = 24;
    int cols = 80;
    term_size(&rows, &cols);

    Input input;
    input_init(&input);

    Config cfg;
    if (load_config(&cfg) != 0) {
        die("failed to load config");
    }
    dbg_log(&dbg, "debug sidebar aktiv (%dx%d)", cols, rows);

    AppState state = {
        .cmd_active = false,
        .confirm_quit = false,
        .models_dialog = false,
        .settings_dialog = false,
    };

    draw(rows, cols, &input, &state, &dbg, &cfg);

    bool quit = false;
    while (!quit) {
        Key k = key_read();

        bool dirty = (g_resized != 0);
        if (g_resized) {
            g_resized = 0;
            term_size(&rows, &cols);
            fputs("\x1b[2J", stdout);
        }

        if (k.kind == KEY_CTRL_C) {
            if (state.confirm_quit) {
                quit = true;
            } else {
                state.confirm_quit = true;
                dirty = true;
            }
        } else if (k.kind == KEY_CTRL_Q) {
            quit = true;
        } else if (k.kind == KEY_NONE) {
            /* nur resize-interesse */
        } else if (ui_mode(&state) != MODE_INPUT) {
            /* dialog-modus: tasten gehen nicht ins eingabefeld.
             * jede dialog-schliessende taste kehrt zum chat zurueck,
             * alles andere wird ignoriert (spaeter: cursor-navigation). */
            switch (k.kind) {
            case KEY_ESCAPE:
            case KEY_ENTER:
            case KEY_NEWLINE:
                state.models_dialog = false;
                state.settings_dialog = false;
                state.confirm_quit = false;
                dirty = true;
                break;
            default:
                if (state.confirm_quit) {
                    state.confirm_quit = false;
                    dirty = true;
                }
                break;
            }
        } else {
            if (state.confirm_quit) {
                state.confirm_quit = false;
                dirty = true;
            }
            switch (k.kind) {
            case KEY_NEWLINE:
                input_newline(&input, rows,
                              cmd_list_height(&input, state.cmd_active),
                              state.confirm_quit);
                dirty = true;
                break;
            case KEY_ENTER: {
                char word[64];
                char args[128];
                cmd_parse(&input, word, sizeof word, args, sizeof args);

                if (word[0] == '/') {
                    switch (cmd_lookup(word)) {
                    case CMD_CLEAR:
                        input_reset(
                            &input); /* draw() schreibt eh jeden frame
                                      * komplett, "clear" = input leeren */
                        break;
                    case CMD_MODELS:
                        state.models_dialog = true;
                        input_reset(&input);
                        break;
                    case CMD_QUIT:
                        quit = true;
                        break;
                    case CMD_SETTINGS:
                        state.settings_dialog = true;
                        input_reset(&input);
                        break;
                    default:
                        dbg_log(&dbg, "unbekannter befehl: '%s'", word);
                        input_reset(&input); /* fehlerhafte eingabe wegwerfen */
                        break;
                    }
                    state.cmd_active = false;
                    dirty = true;
                } else {
                    /* normale nachricht: bestehendes verhalten */
                    input_reset(&input);
                    state.cmd_active = false;
                    dirty = true;
                }
                break;
            }
            case KEY_BACKSPACE:
                input_backspace(&input);
                state.cmd_active = input_in_cmd(&input);
                dirty = true;
                break;
            case KEY_CHAR:
                input_char(&input, k.ch, main_width(cols));
                state.cmd_active = input_in_cmd(&input);
                dirty = true;
                break;
            case KEY_ESCAPE:
                input_reset(&input);
                state.cmd_active = input_in_cmd(&input);
                dirty = true;
                break;
            case KEY_NONE:
            case KEY_CTRL_C:
            case KEY_CTRL_Q:
                break;
            }
        }

        if (dirty && !quit) {
            draw(rows, cols, &input, &state, &dbg, &cfg);
        }
    }

    input_free(&input);
    free_config(&cfg);
    return 0;
}
