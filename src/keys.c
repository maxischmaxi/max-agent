#include "keys.h"

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <unistd.h>

#include "chat.h"
#include "command.h"
#include "config.h"
#include "debug.h"
#include "draw.h"
#include "history.h"
#include "send.h"
#include "settings.h"
#include "theme.h"
#include "utils.h"

/* tastatur-puffer ist privat fuer dieses modul */
static char g_pending[SEQ_MAX];
static size_t g_pending_len = 0;

void keys_unread(const char *buf, size_t len)
{
    if (buf == NULL) {
        return;
    }
    size_t space = SEQ_MAX - g_pending_len;
    if (len > space) {
        len = space; /* puffer voll: ueberschuss verwerfen */
    }
    /* vorhandene bytes nach hinten schieben, neue vorne anstellen */
    memmove(g_pending + len, g_pending, g_pending_len);
    memcpy(g_pending, buf, len);
    g_pending_len += len;
}

static bool is_csi_final(char c)
{
    unsigned char u = (unsigned char)c;
    if (u >= 0x40 && u <= 0x7e) {
        return true;
    }
    return false;
}

/* ------------------------------------------------------------------ */
/* POSIX/readline-shortcuts: eine tabelle fuer beide terminal-        */
/* encodings. legacy terminals senden das nackte ctrl-byte (raw mode),*/
/* kitty-protokoll sendet "\x1b[<codepoint>;5u" (5 = ctrl-modifikator, */
/* codepoint = kleinbuchstabe). ctrl+c/ctrl+q sind app-spezifisch,     */
/* laufen aber ueber dasselbe encoding und stehen deshalb mit in der */
/* tabelle.                                                           */
/* ------------------------------------------------------------------ */
typedef struct {
    unsigned char byte; /* ctrl-byte im legacy-encoding */
    int cp;             /* kitty-codepoint (kleinbuchstabe) */
    KeyKind kind;
} CtrlKey;

static const CtrlKey CTRL_KEYS[] = {
    {0x01, 'a', KEY_CTRL_A}, {0x02, 'b', KEY_CTRL_B}, {0x04, 'd', KEY_CTRL_D},
    {0x05, 'e', KEY_CTRL_E}, {0x06, 'f', KEY_CTRL_F}, {0x08, 'h', KEY_CTRL_H},
    {0x0b, 'k', KEY_CTRL_K}, {0x0c, 'l', KEY_CTRL_L}, {0x0e, 'n', KEY_CTRL_N},
    {0x10, 'p', KEY_CTRL_P}, {0x14, 't', KEY_CTRL_T}, {0x15, 'u', KEY_CTRL_U},
    {0x17, 'w', KEY_CTRL_W}, {0x03, 'c', KEY_CTRL_C}, /* app: quit-confirm /
                                                         dialog zu */
    {0x11, 'q', KEY_CTRL_Q},                          /* app: sofort beenden */
};

static KeyKind ctrl_from_byte(unsigned char c)
{
    size_t n = sizeof CTRL_KEYS / sizeof CTRL_KEYS[0];
    for (size_t i = 0; i < n; i++) {
        if (CTRL_KEYS[i].byte == c) {
            return CTRL_KEYS[i].kind;
        }
    }
    return KEY_NONE;
}

static KeyKind ctrl_from_kitty(int cp)
{
    size_t n = sizeof CTRL_KEYS / sizeof CTRL_KEYS[0];
    for (size_t i = 0; i < n; i++) {
        if (CTRL_KEYS[i].cp == cp) {
            return CTRL_KEYS[i].kind;
        }
    }
    return KEY_NONE;
}

/* ------------------------------------------------------------------ */
/* Meta-bindings (alt+x). legacy-terminals senden ESC + zeichen, das  */
/* kitty-protokoll CSI <codepoint>;3u (3 = alt-modifikator).          */
/* ------------------------------------------------------------------ */
static const struct {
    unsigned char byte; /* legacy: byte NACH ESC */
    int cp;             /* kitty-codepoint */
    KeyKind kind;
} ALT_KEYS[] = {
    {'b', 'b', KEY_ALT_B}, {'f', 'f', KEY_ALT_F},
    {'d', 'd', KEY_ALT_D}, {0x7f, 127, KEY_ALT_BACKSPACE},
    {'t', 't', KEY_ALT_T}, {'u', 'u', KEY_ALT_U},
    {'l', 'l', KEY_ALT_L}, {'c', 'c', KEY_ALT_C},
};

static KeyKind alt_from_byte(unsigned char c)
{
    size_t n = sizeof ALT_KEYS / sizeof ALT_KEYS[0];
    /* alt+shift liefert grossbuchstaben: wie die kleingeschriebene
     * variante behandeln (readline macht dasselbe) */
    if (c >= 'A' && c <= 'Z') {
        c = (unsigned char)(c + ('a' - 'A'));
    }
    for (size_t i = 0; i < n; i++) {
        if (ALT_KEYS[i].byte == c) {
            return ALT_KEYS[i].kind;
        }
    }
    return KEY_NONE;
}

static KeyKind alt_from_kitty(int cp)
{
    size_t n = sizeof ALT_KEYS / sizeof ALT_KEYS[0];
    if (cp >= 'A' && cp <= 'Z') {
        cp = cp + ('a' - 'A');
    }
    for (size_t i = 0; i < n; i++) {
        if (ALT_KEYS[i].cp == cp) {
            return ALT_KEYS[i].kind;
        }
    }
    return KEY_NONE;
}

static bool wait_readable(int timeout_ms)
{
    fd_set fds;
    struct timeval tv;
    FD_ZERO(&fds);
    FD_SET(STDIN_FILENO, &fds);
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (long)(timeout_ms % 1000) * 1000L;
    int r = select(STDIN_FILENO + 1, &fds, NULL, NULL, &tv);
    if (r > 0) {
        return true;
    }
    return false;
}

/* steckt in diesen bytes ein abbruch?
 *
 * ctrl+c (0x03) ist eindeutig. escape dagegen ist das erste byte
 * JEDER sequenz – pfeiltasten, pos1/ende, f-tasten. wer waehrend der
 * antwort pfeil-hoch drueckt, will nicht abbrechen. escape zaehlt
 * deshalb nur, wenn nichts darauf folgt.
 *
 * die grenze: kaeme eine sequenz zerstueckelt an (erst 0x1b, der
 * rest im naechsten read), wuerde das erste byte als escape gelten.
 * terminals schicken sequenzen praktisch immer am stueck. */
static bool has_abort(const char *buf, size_t len)
{
    size_t i = 0;
    while (i < len) {
        if (buf[i] == 0x03) {
            return true;
        }
        if (buf[i] == 0x1b) {
            if (len - i == 1) {
                return true; /* escape steht allein */
            }
            size_t seq = key_seq_len(buf + i, len - i);
            if (seq == 0) {
                return false; /* sequenz unvollstaendig: abwarten */
            }
            i += seq; /* ganze sequenz ueberspringen */
            continue;
        }
        i++;
    }
    return false;
}

bool keys_abort_pressed(void)
{
    /* was schon im puffer liegt, zuerst: es ist aelter als stdin */
    if (has_abort(g_pending, g_pending_len)) {
        g_pending_len = 0; /* rest verwerfen, siehe keys.h */
        return true;
    }
    if (!wait_readable(0)) {
        return false;
    }
    char buf[SEQ_MAX];
    ssize_t n = read(STDIN_FILENO, buf, sizeof buf);
    if (n <= 0) {
        return false;
    }
    if (has_abort(buf, (size_t)n)) {
        g_pending_len = 0;
        return true;
    }
    keys_unread(buf, (size_t)n); /* nichts dabei: alles zurueck */
    return false;
}

/* modifikator-bits aus dem encoding (uebertragen wird 1 + bitmaske:
 * 1 = shift, 2 = alt, 4 = ctrl). caps lock (64) und num lock (128)
 * werden ausmaskiert, sonst faellt jeder shortcut aus, sobald eine
 * lock-taste aktiv ist. */
static unsigned mods_bits(int mods)
{
    if (mods < 1) {
        return 0;
    }
    unsigned bits = (unsigned)(mods - 1);
    return bits & ~(64U | 128U);
}

static KeyKind modified_key(int cp, unsigned bits)
{
    if ((bits & 4U) != 0U) {
        return ctrl_from_kitty(cp);
    }
    if ((bits & 2U) != 0U) {
        return alt_from_kitty(cp);
    }
    return KEY_NONE;
}

/* CSI <codepoint>;<mods> u (kitty) und CSI 27;<mods>;<codepoint> ~
 * (xterm modifyOtherKeys) tragen dieselbe information */
static KeyKind key_from_csi_codepoint(int cp, unsigned bits)
{
    if (cp == 13 && (bits & 7U) != 0U) {
        return KEY_NEWLINE; /* shift/alt/ctrl + enter */
    }
    if (cp == 106 && (bits & 4U) != 0U) {
        return KEY_NEWLINE; /* ctrl+j */
    }
    if (bits == 0U) {
        /* unmodifizierte tasten, die das kitty-protokoll ebenfalls
         * als CSI u meldet */
        if (cp == 9) {
            return KEY_TAB;
        }
        if (cp == 13) {
            return KEY_ENTER;
        }
        if (cp == 27) {
            return KEY_ESCAPE;
        }
        return KEY_NONE;
    }
    return modified_key(cp, bits);
}

Key key_from_escape(const char *seq, ssize_t len)
{
    Key k = {KEY_NONE, 0};

    if (len == 1) {
        /* einzelnes esc ohne nachfolgende bytes */
        k.kind = KEY_ESCAPE;
        return k;
    }
    if (len == 2 && seq[1] == '\r') {
        k.kind = KEY_NEWLINE;
        return k;
    }
    if (len == 2) {
        /* meta-binding im legacy-encoding: ESC + zeichen */
        k.kind = alt_from_byte((unsigned char)seq[1]);
        if (k.kind != KEY_NONE) {
            return k;
        }
    }
    if (len < 3 || seq[1] != '[') {
        return k;
    }

    int p[3] = {-1, -1, -1};
    char final = 0;
    int idx = 0;
    for (ssize_t i = 2; i < len; i++) {
        char c = seq[i];
        if (c >= '0' && c <= '9') {
            if (p[idx] == -1) {
                p[idx] = 0;
            }
            p[idx] = (p[idx] * 10) + (c - '0');
        } else if (c == ';') {
            if (idx < 2) {
                idx++;
            }
        } else if (is_csi_final(c)) {
            final = c;
            break;
        } else {
            return k;
        }
    }
    if (final == 0) {
        return k;
    }

    if (final == 'A' && len == 3) {
        k.kind = KEY_UP;
        return k;
    }
    if (final == 'B' && len == 3) {
        k.kind = KEY_DOWN;
        return k;
    }

    /* page up/down: CSI 5~ / CSI 6~ (legacy-encoding; die app
     * aktiviert das kitty-protokoll nie, daher kommt hier immer
     * die klassische form an). mit modifikatoren: uninteressant. */
    if (final == '~' && p[0] == 5 && p[1] < 0) {
        k.kind = KEY_PGUP;
        return k;
    }
    if (final == '~' && p[0] == 6 && p[1] < 0) {
        k.kind = KEY_PGDN;
        return k;
    }

    if (final == 'u') {
        /* kitty-protokoll: CSI <codepoint>;<mods> u. shift+tab
         * (9;2u bzw. CSI Z) bleibt frei, z.B. fuer spaeteres
         * reverse-completion */
        k.kind = key_from_csi_codepoint(p[0], mods_bits(p[1]));
    } else if (final == '~' && p[0] == 27 && p[2] >= 0) {
        /* xterm modifyOtherKeys: CSI 27;<mods>;<codepoint> ~ */
        k.kind = key_from_csi_codepoint(p[2], mods_bits(p[1]));
    }
    return k;
}

Key key_from_byte(char c)
{
    Key k = {KEY_NONE, 0};
    if (c == '\r') {
        k.kind = KEY_ENTER;
    } else if (c == '\n') {
        k.kind = KEY_NEWLINE;
    } else if (c == 0x7f) {
        k.kind = KEY_BACKSPACE;
    } else if (c == '\t') {
        /* tab: im raw mode kommt kein escape-code, sondern das
         * nackte byte 0x09 */
        k.kind = KEY_TAB;
    } else if (c >= 32 && c <= 126) {
        k.kind = KEY_CHAR;
        k.ch = c;
    } else {
        /* ctrl-shortcuts aus dem steuerzeichen-bereich (tabelle) */
        k.kind = ctrl_from_byte((unsigned char)c);
    }
    return k;
}

size_t key_seq_len(const char *buf, size_t len)
{
    if (len == 0) {
        return 0;
    }
    if (buf[0] != 0x1b) {
        return 1; /* normales zeichen */
    }
    if (len == 1) {
        return 0; /* nur esc: der rest koennte noch unterwegs sein */
    }
    if (buf[1] == 0x1b) {
        return 1; /* esc esc: das erste esc ist fuer sich fertig */
    }
    if (buf[1] == '[') {
        for (size_t i = 2; i < len; i++) {
            if (is_csi_final(buf[i])) {
                return i + 1;
            }
        }
        return 0; /* final-byte fehlt noch */
    }
    if (buf[1] == 'O') {
        return (len >= 3) ? 3 : 0; /* SS3 */
    }
    return 2; /* meta-binding: esc + zeichen */
}

typedef enum {
    FILL_OK,   /* neue bytes im puffer */
    FILL_NONE, /* unterbrochen, z.B. SIGWINCH */
    FILL_EOF,  /* pane zu */
} FillResult;

static FillResult fill_pending(void)
{
    if (g_pending_len >= SEQ_MAX) {
        g_pending_len = 0; /* voller puffer ohne gueltige sequenz */
    }
    ssize_t n =
        read(STDIN_FILENO, g_pending + g_pending_len, SEQ_MAX - g_pending_len);
    if (n < 0) {
        if (errno == EINTR) {
            return FILL_NONE;
        }
        die("read failed");
    }
    if (n == 0) {
        return FILL_EOF;
    }
    g_pending_len += (size_t)n;
    return FILL_OK;
}

/* die ersten seq_len bytes als eine taste auswerten und entfernen */
static Key take_pending(size_t seq_len)
{
    Key k = (g_pending[0] == 0x1b)
                ? key_from_escape(g_pending, (ssize_t)seq_len)
                : key_from_byte(g_pending[0]);
    g_pending_len -= seq_len;
    memmove(g_pending, g_pending + seq_len, g_pending_len);
    return k;
}

/* wartezeiten fuer unvollstaendige sequenzen (ms): ein einzelnes esc
 * ist nach kurzer pause wirklich die escape-taste. eine angefangene
 * CSI-sequenz ist dagegen nur unterwegs (langsame verbindung, voller
 * puffer) und bekommt deutlich mehr zeit - sonst landen ihre bytes
 * als text im input. */
#define ESC_WAIT_MS  30
#define SEQ_WAIT_MS  200
#define WAIT_STEP_MS 10

/* eine taste aus dem puffer holen, bei leerem puffer von stdin
 * nachlesen. enthaelt EIN read mehrere tasten (tastenwiederholung,
 * schnelles tippen, paste), bleibt der rest im puffer und wird beim
 * naechsten aufruf ausgewertet - es geht nichts verloren. */
Key key_read(void)
{
    int waited = 0;
    for (;;) {
        if (g_pending_len > 0) {
            size_t seq_len = key_seq_len(g_pending, g_pending_len);
            if (seq_len > 0) {
                return take_pending(seq_len);
            }
            int budget = (g_pending_len == 1) ? ESC_WAIT_MS : SEQ_WAIT_MS;
            if (waited >= budget) {
                /* es kommt nichts mehr: das bisherige als ganzes
                 * werten (einzelnes esc, abgebrochene sequenz) */
                return take_pending(g_pending_len);
            }
            if (!wait_readable(WAIT_STEP_MS)) {
                waited += WAIT_STEP_MS;
                continue;
            }
        }
        FillResult r = fill_pending();
        if (r == FILL_EOF) {
            return (Key){KEY_CTRL_Q, 0};
        }
        if (r == FILL_NONE) {
            return (Key){KEY_NONE, 0}; /* EINTR: resize pruefen */
        }
        waited = 0; /* fortschritt: die wartezeit beginnt von vorn */
    }
}

/* ------------------------------------------------------------------ */
/* history-navigation: der entwurf im feld wird beim ersten schritt   */
/* gesichert, danach ersetzt jeder eintrag die eingabe komplett.      */
/* liefert die history NULL, bleibt die eingabe unveraendert (am      */
/* aeltesten eintrag, oder wenn gar nicht geblaettert wird).          */
/* ------------------------------------------------------------------ */

static void history_apply(AppState *state, Input *input, const char *entry)
{
    if (entry == NULL) {
        return;
    }
    input_set_text(input, entry);
    state->cmd_active = input_in_cmd(input);
    state->dirty = true;
}

static void history_back(AppState *state, Input *input)
{
    char *current = chat_flatten_input(input); /* NULL = leeres feld */
    const char *entry = history_prev(&state->history, current);
    free(current);
    history_apply(state, input, entry);
}

static void history_forward(AppState *state, Input *input)
{
    history_apply(state, input, history_next(&state->history));
}

static void handle_ctrl_c(AppState *state, DebugState *dbg, const Config *cfg)
{
    (void)dbg; /* dbg_log: im release wegkompiliert */
    dbg_log(dbg, "input count: %d", (int)state->input.count);

    if (!state->models_dialog && !state->settings_dialog &&
        state->input.count > 1) {
        input_reset(&state->input);
        state->dirty = true;
        return;
    }

    if (!state->models_dialog && !state->settings_dialog &&
        state->input.count == 1) {
        char *last_line = state->input.lines[state->input.count - 1];
        size_t len = strlen(last_line);
        if (len > 0) {
            input_reset(&state->input);
            state->dirty = true;
            return;
        }
    }

    /* in einem dialog schliesst ctrl+c erst den dialog */
    if (state->models_dialog || state->settings_dialog) {
        state->models_dialog = false;
        state->settings_dialog = false;
        state->theme_sub = false;
        state->dialog = (DialogState){0};
        state->confirm_quit = false;
        state->dirty = true;
        return;
    }

    /* setting "confirm quit": aus -> ctrl+c beendet sofort */
    if (!cfg->confirm_quit) {
        state->quit = true;
        return;
    }

    if (state->confirm_quit) {
        state->quit = true;
    } else {
        state->confirm_quit = true;
        state->dirty = true;
    }
}

static void handle_settings(AppState *state, Config *cfg, DebugState *dbg,
                            Key k)
{
    /* suchen + cursor: bei allen dialogs identisch */
    if (dialog_navigate(state, k)) {
        state->dirty = true;
        return;
    }

    switch (k.kind) {
    case KEY_ESCAPE:
        state->settings_dialog = false;
        state->theme_sub = false;
        state->dialog = (DialogState){0};
        state->confirm_quit = false;
        state->dirty = true;
        break;
    case KEY_ENTER:
    case KEY_NEWLINE: {
        /* was enter tut, entscheidet der typ des eintrags */
        int hits[DIALOG_MATCH_MAX];
        int n = names_match(SETTING_NAMES, SET_COUNT, state->dialog.search,
                            hits, DIALOG_MATCH_MAX);
        if (n == 0 || state->dialog.selected >= n) {
            break; /* kein treffer: nichts zu tun */
        }
        switch ((SettingId)hits[state->dialog.selected]) {
        case SET_THEME:
            /* submenu: theme-optionen, frische suche */
            state->theme_sub = true;
            state->dialog = (DialogState){0};
            state->dirty = true;
            break;
        case SET_SYSTEM_PROMPT:
            /* submenu: default / off / edit, frische suche */
            state->prompt_sub = true;
            state->dialog = (DialogState){0};
            state->dirty = true;
            break;
        case SET_CONFIRM_QUIT:
            /* boolean: enter schaltet nur um, dialog bleibt offen,
             * damit man den neuen wert direkt sieht */
            if (cfg->confirm_quit) {
                cfg->confirm_quit = false;
            } else {
                cfg->confirm_quit = true;
            }
            dbg_log(dbg, "confirm quit: %s", on_off(cfg->confirm_quit));
            config_persist(cfg, dbg);
            state->dirty = true;
            break;
        case SET_COUNT:
            break;
        }
        break;
    }
    default:
        break;
    }
}

static void handle_theme(AppState *state, Config *cfg, DebugState *dbg, Key k)
{
    if (dialog_navigate(state, k)) {
        state->dirty = true;
        return;
    }

    switch (k.kind) {
    case KEY_ESCAPE:
        /* zurueck in die settings-liste (nicht dialog schliessen) */
        state->theme_sub = false;
        state->dialog = (DialogState){0};
        state->confirm_quit = false;
        state->dirty = true;
        break;
    case KEY_ENTER:
    case KEY_NEWLINE: {
        /* theme live anwenden; dialog bleibt offen, damit man
         * mehrere themes direkt ausprobieren kann. esc geht zurueck
         * in die settings-liste. */
        const char *names[16];
        int total = theme_names(names, (int)(sizeof names / sizeof names[0]));
        int hits[DIALOG_MATCH_MAX];
        int n = names_match(names, total, state->dialog.search, hits,
                            DIALOG_MATCH_MAX);
        if (n > 0 && state->dialog.selected < n) {
            const char *name = names[hits[state->dialog.selected]];
            if (theme_select(name)) {
                free(cfg->theme);
                cfg->theme = dup_str(name);
                if (cfg->theme == NULL) {
                    die("out of memory");
                }
                dbg_log(dbg, "theme: %s (match=%s)", theme_current()->name,
                        theme_current()->match);
                config_persist(cfg, dbg);
                state->dirty = true; /* farben gelten sofort */
            }
        }
        break;
    }
    default:
        break;
    }
}

/* system-prompt-untermenue. "default" und "off" setzen die config
 * direkt (der dialog bleibt offen, man sieht den neuen zustand
 * sofort); "edit" schliesst den dialog und uebergibt an das
 * eingabefeld, das den vollen zeilen-editor mitbringt. */
static void handle_prompt(AppState *state, Config *cfg, DebugState *dbg, Key k)
{
    if (dialog_navigate(state, k)) {
        state->dirty = true;
        return;
    }

    switch (k.kind) {
    case KEY_ESCAPE:
        /* zurueck in die settings-liste, nicht dialog schliessen */
        state->prompt_sub = false;
        state->dialog = (DialogState){0};
        state->confirm_quit = false;
        state->dirty = true;
        break;
    case KEY_ENTER:
    case KEY_NEWLINE: {
        int hits[DIALOG_MATCH_MAX];
        int n = names_match(PROMPT_OPT_NAMES, PROMPT_OPT_COUNT,
                            state->dialog.search, hits, DIALOG_MATCH_MAX);
        if (n == 0 || state->dialog.selected >= n) {
            break; /* kein treffer: nichts zu tun */
        }
        switch ((PromptOpt)hits[state->dialog.selected]) {
        case PROMPT_DEFAULT:
            free(cfg->system_prompt);
            cfg->system_prompt = NULL; /* eingebaute vorlage */
            dbg_log(dbg, "system-prompt: default");
            config_persist(cfg, dbg);
            state->dirty = true;
            break;
        case PROMPT_OFF:
            free(cfg->system_prompt);
            cfg->system_prompt = dup_str(""); /* bewusst keiner */
            if (cfg->system_prompt == NULL) {
                die("out of memory");
            }
            dbg_log(dbg, "system-prompt: aus");
            config_persist(cfg, dbg);
            state->dirty = true;
            break;
        case PROMPT_EDIT:
            /* dialog zu, eingabefeld auf. steht schon ein eigener
             * text in der config, wird er zum bearbeiten vorgelegt;
             * bei default/aus faengt man leer an. */
            state->settings_dialog = false;
            state->prompt_sub = false;
            state->dialog = (DialogState){0};
            state->prompt_edit = true;
            input_set_text(&state->input, (cfg->system_prompt != NULL &&
                                           cfg->system_prompt[0] != '\0')
                                              ? cfg->system_prompt
                                              : "");
            state->cmd_active = false;
            state->dirty = true;
            break;
        case PROMPT_OPT_COUNT:
            break;
        }
        break;
    }
    default:
        break;
    }
}

static void handle_models(AppState *state, Config *cfg, DebugState *dbg, Key k)
{
    /* dialog-modus: suchtext ist der input, das chat-feld
     * existiert hier nicht */
    if (dialog_navigate(state, k)) {
        state->dirty = true;
        return;
    }

    switch (k.kind) {
    case KEY_ESCAPE:
        state->models_dialog = false;
        state->dialog = (DialogState){0};
        state->confirm_quit = false;
        state->dirty = true;
        break;
    case KEY_ENTER:
    case KEY_NEWLINE: {
        /* treffer uebernehmen: layout-daten sind nur im draw
         * gueltig -> hier nochmal filtern (billig), selected
         * ist von layout_compute() schon angeglichen */
        int hits[DIALOG_MATCH_MAX];
        int n = models_match(cfg, state->dialog.search, hits, DIALOG_MATCH_MAX);
        if (n > 0 && state->dialog.selected < n) {
            const char *url = "";
            const Model *m = model_at(cfg, hits[state->dialog.selected], &url);
            if (m != NULL && m->id != NULL) {
                free(cfg->active_model);
                cfg->active_model = dup_str(m->id);
                if (cfg->active_model == NULL) {
                    die("out of memory");
                }
                dbg_log(dbg, "modell gewaehlt: %s", m->id);
                config_persist(cfg, dbg);
            }
        }
        state->models_dialog = false;
        state->dialog = (DialogState){0};
        state->dirty = true;
        break;
    }
    default:
        break;
    }
}

static void autocomplete_command(Input *input, int id, int max_len)
{
    const char *name = COMMANDS[id].name;
    char *last = input->lines[input->count - 1];
    const char *word = last_word(input);
    size_t keep = (size_t)(word - last); /* text VOR dem wort */
    size_t name_len = strlen(name);

    /* breitengrenze wie input_char: nichts erzwingen */
    if (keep + 1 + name_len >= (size_t)max_len) {
        return;
    }

    char *grown = realloc(last, keep + 1 + name_len + 1);
    if (!grown) {
        die("out of memory");
    }
    memcpy(grown + keep + 1, name, name_len + 1); /* '/' + name + '\0' */
    input->lines[input->count - 1] = grown;
    /* der befehl steht immer in der letzten zeile – der cursor
     * gehoert danach hinter den neuen text */
    input->cursor_line = input->count - 1;
    input_cursor_end(input);
}

/* ------------------------------------------------------------------ */
/* stream-redraw: send_stream ruft nach chunks zurueck, damit die   */
/* antwort live waechst. der callback braucht rows/cols/cfg – die   */
/* passen in einen struct, weil OaiStreamCallbacks nur einen void*   */
/* als user_data hat.                                              */
/* ------------------------------------------------------------------ */
typedef struct {
    AppState *state;
    Config *cfg;
    DebugState *dbg;
    int rows;
    int cols;
} StreamRedrawCtx;

static void stream_redraw(void *ud)
{
    StreamRedrawCtx *rc = ud;
    draw(rc->rows, rc->cols, rc->state, rc->dbg, rc->cfg);
}

/* rueckfrage vor einem tool. haelt den agent-loop an, zeichnet die
 * frage in die zeile des thinking-indikators und wartet auf eine
 * taste. der aufgerufene tool-call steht schon im verlauf darueber,
 * man sieht also, worum es geht.
 *
 * [j]a fuehrt einmal aus, [n]ein lehnt ab, [a]lle schaltet die
 * rueckfrage fuer den rest der sitzung ab. escape und ctrl+c
 * gelten als nein – wer abbricht, will nichts ausfuehren. */
static bool confirm_tool(const char *name, const char *arguments, void *ud)
{
    StreamRedrawCtx *rc = ud;
    AppState *st = rc->state;
    (void)arguments; /* steht schon als tool-call im verlauf */

    if (st->tools_always) {
        return true;
    }

    snprintf(st->tool_ask, sizeof st->tool_ask, "%s",
             (name != NULL) ? name : "?");
    bool allow = false;
    for (;;) {
        draw(rc->rows, rc->cols, st, rc->dbg, rc->cfg);
        Key k = key_read();
        if (k.kind == KEY_ESCAPE || k.kind == KEY_CTRL_C ||
            k.kind == KEY_CTRL_Q) {
            break; /* abbruch = nein */
        }
        if (k.kind != KEY_CHAR) {
            continue; /* alles andere ignorieren, weiter fragen */
        }
        if (k.ch == 'j' || k.ch == 'y') {
            allow = true;
            break;
        }
        if (k.ch == 'a') {
            st->tools_always = true;
            allow = true;
            break;
        }
        if (k.ch == 'n') {
            break;
        }
    }
    st->tool_ask[0] = '\0'; /* frage wieder weg */
    st->dirty = true;
    return allow;
}

static int cmd_list_height(const AppState *st)
{
    if (!st->cmd_active) {
        return 0;
    }
    char prefix[64];
    int idx[COMMAND_COUNT];
    cmd_prefix(&st->input, prefix, sizeof prefix);
    int n = cmd_match(prefix, idx, COMMAND_COUNT);
    return (n > 0) ? n : 1; /* kein treffer: hinweis-zeile reservieren */
}

static void cmd_parse(const AppState *st, char *word, size_t word_sz,
                      char *args, size_t args_sz)
{
    word[0] = '\0';
    args[0] = '\0';

    const char *last = st->input.lines[st->input.count - 1];
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

static void handle_all(AppState *state, Config *cfg, DebugState *dbg, int rows,
                       int cols, Key k)
{
    /* kurzform: die chat-eingabe liegt als member im state */
    Input *input = &state->input;

    if (state->confirm_quit) {
        state->confirm_quit = false;
        state->dirty = true;
    }

    switch (k.kind) {
    case KEY_NEWLINE:
        input_newline(input, rows, cmd_list_height(state), state->confirm_quit);
        state->dirty = true;
        break;
    case KEY_ENTER: {
        char word[64];
        char args[128];
        cmd_parse(state, word, sizeof word, args, sizeof args);

        /* system-prompt bearbeiten: enter speichert und geht zurueck
         * in den chat. der text landet NICHT in der history – das
         * ist keine nachricht an das modell. leeres feld heisst
         * hier "zurueck zur eingebauten vorlage", nicht "aus": wer
         * den prompt abschalten will, nimmt die option "off". */
        if (state->prompt_edit) {
            char *text = chat_flatten_input(input);
            free(cfg->system_prompt);
            cfg->system_prompt = text; /* NULL = wieder default */
            dbg_log(dbg, "system-prompt: %s",
                    (text != NULL) ? "eigener text" : "default");
            config_persist(cfg, dbg);
            input_reset(input);
            state->prompt_edit = false;
            state->cmd_active = false;
            state->dirty = true;
            break;
        }

        /* alles abgeschickte kommt in die history – auch befehle,
         * die will man genauso wiederholen. muss VOR der
         * verzweigung passieren: cmd_clear() leert die eingabe. */
        char *entry = chat_flatten_input(input);
        history_add(&state->history, entry);
        free(entry);

        if (word[0] == '/') {
            switch (cmd_lookup(word)) {
            case CMD_CLEAR:
                cmd_clear(state);
                break;
            case CMD_MODELS:
                cmd_models(state);
                break;
            case CMD_QUIT:
                state->quit = true;
                break;
            case CMD_SETTINGS:
                cmd_settings(state);
                break;
            default: {
                char prefix[64];
                int idx[COMMAND_COUNT];
                cmd_prefix(input, prefix, sizeof(prefix));
                int m = cmd_match(prefix, idx, COMMAND_COUNT);
                if (m > 0) {
                    switch (idx[0]) {
                    case CMD_CLEAR:
                        cmd_clear(state);
                        break;
                    case CMD_MODELS:
                        cmd_models(state);
                        break;
                    case CMD_SETTINGS:
                        cmd_settings(state);
                        break;
                    default:
                        break;
                    }
                }
                break;
            }
            }
            state->cmd_active = false;
            state->dirty = true;
        } else {
            /* normale nachricht: eingabe wandert ins transcript und
             * wird als STREAM an die api geschickt – die antwort
             * waechst live im verlauf (redraw kommt aus send_stream
             * zurueck, gedrosselt). die UI blockiert bis zum ende:
             * vorher noch einen frame mit thinking-indikator, sonst
             * wirkt die app tot. leere eingabe: nichts senden. */
            char *text = chat_flatten_input(input);
            input_reset(input);
            state->cmd_active = false;
            if (text != NULL) {
                if (chat_append(&state->chat, CHAT_ROLE_USER, text) != 0) {
                    die("out of memory");
                }
                free(text);
                state->chat_scroll = 0; /* neue nachricht: folgen */

                state->busy = true;
                draw(rows, cols, state, dbg, cfg);
                StreamRedrawCtx rc = {
                    .state = state,
                    .cfg = cfg,
                    .dbg = dbg,
                    .rows = rows,
                    .cols = cols,
                };
                SendHooks hooks = {
                    .ctx = &rc,
                    .redraw = stream_redraw,
                    .confirm_tool = confirm_tool,
                };
                (void)send_stream(state, cfg, dbg, &hooks);
                state->busy = false;
            }
            state->dirty = true;
        }
        break;
    }
    case KEY_BACKSPACE:
    case KEY_CTRL_H: /* posix: backward-delete-char = backspace */
        input_backspace(input);
        state->cmd_active = input_in_cmd(input);
        state->dirty = true;
        break;
    case KEY_CHAR:
        input_char(input, k.ch, main_width(cols));
        state->cmd_active = input_in_cmd(input);
        state->dirty = true;
        break;
    case KEY_ESCAPE:
        state->prompt_edit = false;     /* bearbeitung verworfen */
        history_reset(&state->history); /* entwurf ist hinfaellig */
        input_reset(input);
        state->cmd_active = input_in_cmd(input);
        state->dirty = true;
        break;
    case KEY_TAB: {
        if (state->cmd_active) {
            char prefix[64];
            int idx[COMMAND_COUNT];
            cmd_prefix(input, prefix, sizeof(prefix));
            int m = cmd_match(prefix, idx, COMMAND_COUNT);
            if (m > 0) {
                autocomplete_command(input, idx[0], main_width(cols));
            }
        }
        state->dirty =
            true; /* egal ob vervollstaendigt: redraw zeigt den log */
        break;
    }
    /* chat-viewport blaettern: die hoehe des verlaufs entspricht
     * den zeilen ueber der input-box (rows - input-zeilen - rahmen);
     * layout_compute klemmt alles weitere und schreibt zurueck */
    case KEY_PGUP:
    case KEY_PGDN: {
        int page = rows - (int)input->count - 3;
        if (page < 1) {
            page = 1;
        }
        if (k.kind == KEY_PGUP) {
            state->chat_scroll += page;
        } else if (state->chat_scroll > 0) {
            state->chat_scroll -= page;
            if (state->chat_scroll < 0) {
                state->chat_scroll = 0;
            }
        }
        state->dirty = true;
        break;
    }

    /* POSIX/readline-shortcuts: nur im normalen chat-input aktiv
     * (die dialog-handler ignorieren sie). alles laeuft ueber die
     * multiline-faehige cursor-utility aus input.c. */
    case KEY_CTRL_A:
        /* cursor an den anfang der aktuellen zeile */
        input_cursor_home(input);
        state->dirty = true;
        break;
    case KEY_CTRL_E:
        /* cursor an das ende der aktuellen zeile */
        input_cursor_end(input);
        state->dirty = true;
        break;
    case KEY_CTRL_B:
        /* cursor ein zeichen zurueck */
        (void)input_cursor_left(input); /* am anfang: nichts zu tun */
        state->dirty = true;
        break;
    case KEY_CTRL_F:
        /* cursor ein zeichen vor */
        (void)input_cursor_right(input); /* am ende: nichts zu tun */
        state->dirty = true;
        break;
    case KEY_CTRL_W:
        /* letztes wort vor dem cursor loeschen; am zeilenanfang den
         * umbruch davor (multiline) */
        (void)input_kill_last_word(input);
        state->cmd_active = input_in_cmd(input);
        state->dirty = true;
        break;
    case KEY_CTRL_U:
        /* von cursor bis zeilenanfang loeschen (bash) */
        (void)input_kill_line(input);
        state->cmd_active = input_in_cmd(input);
        state->dirty = true;
        break;
    case KEY_CTRL_K:
        /* von cursor bis zeilenende loeschen; am ende den umbruch
         * dahinter (multiline) */
        (void)input_kill_to_end(input);
        state->cmd_active = input_in_cmd(input);
        state->dirty = true;
        break;
    case KEY_CTRL_D:
        /* zeichen UNTER dem cursor loeschen; am zeilenende den
         * umbruch dahinter. bewusst NICHT das terminal-EOF-
         * verhalten – beenden ist ctrl+q. */
        (void)input_delete_forward(input);
        state->cmd_active = input_in_cmd(input);
        state->dirty = true;
        break;
    case KEY_CTRL_L:
        /* bildschirm neu zeichnen (clear + redraw, wie resize) */
        fputs("\x1b[2J", stdout);
        state->dirty = true;
        break;
    case KEY_CTRL_T:
        /* zeichen vor/mit dem cursor vertauschen */
        (void)input_transpose_chars(input);
        state->dirty = true;
        break;

    /* Meta-bindings: wort-operationen (alnum-grenzen, siehe input.h) */
    case KEY_ALT_B:
        /* an den anfang des vorherigen worts */
        (void)input_word_left(input);
        state->dirty = true;
        break;
    case KEY_ALT_F:
        /* ans ende des naechsten worts */
        (void)input_word_right(input);
        state->dirty = true;
        break;
    case KEY_ALT_D:
        /* wort ab cursor vorwaerts killen */
        (void)input_kill_word(input);
        state->cmd_active = input_in_cmd(input);
        state->dirty = true;
        break;
    case KEY_ALT_BACKSPACE:
        /* wort vor dem cursor killen (alnum-grenzen) */
        (void)input_kill_word_back(input);
        state->cmd_active = input_in_cmd(input);
        state->dirty = true;
        break;
    case KEY_ALT_T:
        /* wort vor dem cursor mit dem danach vertauschen */
        (void)input_transpose_words(input);
        state->dirty = true;
        break;
    case KEY_ALT_U:
        /* aktuelles/folgendes wort GROSSschreiben */
        (void)input_word_upcase(input);
        state->dirty = true;
        break;
    case KEY_ALT_L:
        /* aktuelles/folgendes wort kleinschreiben */
        (void)input_word_downcase(input);
        state->dirty = true;
        break;
    case KEY_ALT_C:
        /* aktuelles/folgendes wort kapitalisieren */
        (void)input_word_capitalize(input);
        state->dirty = true;
        break;
    case KEY_UP:
        /* in einer mehrzeiligen eingabe erst die zeile wechseln;
         * erst an der obersten zeile geht es in die history. genau
         * so verhalten sich zsh und fish. */
        if (input->cursor_line > 0) {
            input_cursor_line_set(input, input->cursor_line - 1);
            state->dirty = true;
            break;
        }
        history_back(state, input);
        break;
    case KEY_DOWN:
        if (input->cursor_line + 1 < input->count) {
            input_cursor_line_set(input, input->cursor_line + 1);
            state->dirty = true;
            break;
        }
        history_forward(state, input);
        break;
    case KEY_CTRL_P:
        /* readline: ctrl+p/n sind immer history, auch mitten in
         * einer mehrzeiligen eingabe */
        history_back(state, input);
        break;
    case KEY_CTRL_N:
        history_forward(state, input);
        break;
    case KEY_NONE:
    case KEY_CTRL_C:
    case KEY_CTRL_Q:
        break; /* alle ohne wirkung im chat-modus */
    }
}

void handle_key(AppState *state, Config *cfg, DebugState *dbg, int rows,
                int cols)
{
    Key k = key_read();

    if (k.kind == KEY_CTRL_C) {
        handle_ctrl_c(state, dbg, cfg);
    } else if (k.kind == KEY_CTRL_Q) {
        state->quit = true;
    } else if (k.kind == KEY_NONE) {
        /* nur resize-interesse */
    } else if (ui_mode(state) == MODE_SETTINGS) {
        handle_settings(state, cfg, dbg, k);
    } else if (ui_mode(state) == MODE_THEME) {
        handle_theme(state, cfg, dbg, k);
    } else if (ui_mode(state) == MODE_PROMPT) {
        handle_prompt(state, cfg, dbg, k);
    } else if (ui_mode(state) == MODE_MODELS) {
        handle_models(state, cfg, dbg, k);
    } else {
        handle_all(state, cfg, dbg, rows, cols, k);
    }
}
