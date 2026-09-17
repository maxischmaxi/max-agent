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
#include "session.h"
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

void keys_clear_pending(void)
{
    g_pending_len = 0;
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
            /* kitty-terminals (die app fragt "alle tasten als
             * escape-codes" an) melden ctrl+c als CSI 99;5u und
             * esc als CSI 27u – rohe bytes kommen dann NIE an.
             * die sequenz dekodieren und auf die abbruch-kinds
             * pruefen, sonst laegest die app in jeder anfrage
             * fest, bis der idle-timeout feuert. */
            Key k = key_from_escape(buf + i, (ssize_t)seq);
            if (k.kind == KEY_CTRL_C || k.kind == KEY_ESCAPE) {
                return true;
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

/* codepoint als utf-8 in einen KEY_CHAR-key schreiben (kitty-
 * protokoll meldet zeichen als codepoint, nicht als bytes) */
static void key_set_cp(Key *k, int cp);

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
static Key key_from_csi_codepoint(int cp, unsigned bits)
{
    Key k = {KEY_NONE, {0}};

    if (cp == 13 && (bits & 7U) != 0U) {
        k.kind = KEY_NEWLINE; /* shift/alt/ctrl + enter */
        return k;
    }
    if (cp == 106 && (bits & 4U) != 0U) {
        k.kind = KEY_NEWLINE; /* ctrl+j */
        return k;
    }
    if ((bits == 0U || bits == 1U) && cp >= 32 && cp != 127 && cp <= 0x10FFFF) {
        /* text-codepoint, unmodifiziert oder NUR shift: das ist das
         * zeichen selbst. kitty-terminals mit "alle tasten als
         * escape-codes" melden auch gewoehnliche buchstaben und
         * umlaute so (bits == 0); shift+space oder shift+ziffern
         * laufen hier als das schlichte zeichen (bits == 1).
         * tab, enter und esc sind cp < 32 und treffen nicht ein. */
        k.kind = KEY_CHAR;
        key_set_cp(&k, cp);
        return k;
    }
    if (bits == 0U) {
        /* sondertasten im kitty-encoding */
        if (cp == 9) {
            k.kind = KEY_TAB;
        } else if (cp == 13) {
            k.kind = KEY_ENTER;
        } else if (cp == 27) {
            k.kind = KEY_ESCAPE;
        }
        return k;
    }
    k.kind = modified_key(cp, bits);
    return k;
}

Key key_from_escape(const char *seq, ssize_t len)
{
    Key k = {KEY_NONE, {0}};

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
        k = key_from_csi_codepoint(p[0], mods_bits(p[1]));
    } else if (final == '~' && p[0] == 27 && p[2] >= 0) {
        /* xterm modifyOtherKeys: CSI 27;<mods>;<codepoint> ~ */
        k = key_from_csi_codepoint(p[2], mods_bits(p[1]));
    }
    return k;
}

Key key_from_byte(char c)
{
    Key k = {KEY_NONE, {0}};
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
        k.ch[0] = c;
        k.ch[1] = '\0';
    } else {
        /* ctrl-shortcuts aus dem steuerzeichen-bereich (tabelle) */
        k.kind = ctrl_from_byte((unsigned char)c);
    }
    return k;
}

/* rohe utf-8-folge als KEY_CHAR: umlaute und sz kommen im legacy-
 * encoding als 2-byte-folge an, emoji als 4 bytes. die laenge
 * stammt aus key_seq_len, ist also schon validiert */
Key key_from_utf8(const char *buf, size_t len)
{
    Key k = {KEY_NONE, {0}};
    if (len < 1 || len > 4) {
        return k; /* keine zeichen laenger als 4 bytes */
    }
    memcpy(k.ch, buf, len);
    k.ch[len] = '\0';
    k.kind = KEY_CHAR;
    return k;
}

static void key_set_cp(Key *k, int cp)
{
    char *c = k->ch;
    if (cp < 0x80) {
        c[0] = (char)cp;
        c[1] = '\0';
    } else if (cp < 0x800) {
        c[0] = (char)(0xC0 | (cp >> 6));
        c[1] = (char)(0x80 | (cp & 0x3F));
        c[2] = '\0';
    } else if (cp < 0x10000) {
        c[0] = (char)(0xE0 | (cp >> 12));
        c[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        c[2] = (char)(0x80 | (cp & 0x3F));
        c[3] = '\0';
    } else {
        c[0] = (char)(0xF0 | (cp >> 18));
        c[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        c[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        c[3] = (char)(0x80 | (cp & 0x3F));
        c[4] = '\0';
    }
}

size_t key_seq_len(const char *buf, size_t len)
{
    if (len == 0) {
        return 0;
    }
    if (buf[0] != 0x1b) {
        /* utf-8-folge: ein umlaut/sz sind 2 bytes, emoji bis zu 4.
         * erst wenn sie komplett ist, ist es EIN tastendruck; eine
         * halbe folge wartet auf den rest. kaputte folge = 1 byte
         * muell, der als einzelbyte verworfen wird */
        unsigned char c = (unsigned char)buf[0];
        if (c >= 0xC2U && c <= 0xF4U) {
            size_t need = 2; /* lead-byte-art sagt die folgenlaenge */
            if (c >= 0xE0U) {
                need = 3;
            }
            if (c >= 0xF0U) {
                need = 4;
            }
            if (len < need) {
                return 0; /* folge unvollstaendig: rest kommt noch */
            }
            for (size_t i = 1; i < need; i++) {
                if (((unsigned char)buf[i] & 0xC0U) != 0x80U) {
                    return 1; /* kaputte folge: nur das lead-byte */
                }
            }
            return need;
        }
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
    unsigned char first = (unsigned char)g_pending[0];
    Key k = {KEY_NONE, {0}};
    if (g_pending[0] == 0x1b) {
        k = key_from_escape(g_pending, (ssize_t)seq_len);
    } else if (first >= 0xC2U && first <= 0xF4U) {
        /* umlaut/sz/emoji: rohe utf-8-folge */
        k = key_from_utf8(g_pending, seq_len);
    } else {
        k = key_from_byte(g_pending[0]);
    }
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
            return (Key){KEY_CTRL_Q, {0}};
        }
        if (r == FILL_NONE) {
            return (Key){KEY_NONE, {0}}; /* EINTR: resize pruefen */
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

static void handle_ctrl_c(AppState *state, const Config *cfg)
{

    if (!state->models_dialog && !state->settings_dialog &&
        !state->sessions_dialog && state->input.count > 1) {
        input_reset(&state->input);
        state->dirty = true;
        return;
    }

    if (!state->models_dialog && !state->settings_dialog &&
        !state->sessions_dialog && state->input.count == 1) {
        char *last_line = state->input.lines[state->input.count - 1];
        size_t len = strlen(last_line);
        if (len > 0) {
            input_reset(&state->input);
            state->dirty = true;
            return;
        }
    }

    /* in einem dialog schliesst ctrl+c erst den dialog */
    if (state->models_dialog || state->settings_dialog ||
        state->sessions_dialog) {
        state->models_dialog = false;
        state->settings_dialog = false;
        state->sessions_dialog = false;
        state->theme_sub = false;
        state->dialog = (DialogState){0};
        session_list_free(&state->sessions); /* liste nur fuer den dialog */
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

static void handle_settings(AppState *state, Config *cfg, Key k)
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
            config_persist(cfg);
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

static void handle_theme(AppState *state, Config *cfg, Key k)
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
                config_persist(cfg);
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
static void handle_prompt(AppState *state, Config *cfg, Key k)
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
            config_persist(cfg);
            (void)session_prompt_changed(&state->session, NULL);
            state->dirty = true;
            break;
        case PROMPT_OFF:
            free(cfg->system_prompt);
            cfg->system_prompt = dup_str(""); /* bewusst keiner */
            if (cfg->system_prompt == NULL) {
                die("out of memory");
            }
            config_persist(cfg);
            (void)session_prompt_changed(&state->session, "");
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

static void handle_models(AppState *state, Config *cfg, Key k)
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
        /* treffer uebernehmen: die dialog-normalisierung laeuft im
         * draw (dock_build) -> hier nochmal filtern (billig),
         * selected ist dort schon angeglichen */
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
                config_persist(cfg);
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

/* ------------------------------------------------------------------ */
/* resume-dialog: session-liste als unten angedockte box, wie alle */
/* dialogs. suchen/cursor via dialog_navigate; enter laedt die ge-  */
/* waehlte session zurueck in den chat, esc laesst alles unberuehrt */
/* ------------------------------------------------------------------ */

static void sessions_dialog_close(AppState *state)
{
    state->sessions_dialog = false;
    state->dialog = (DialogState){0};
    state->confirm_quit = false;
    session_list_free(&state->sessions); /* nur der dialog liest sie */
    state->dirty = true;
}

static void handle_sessions(AppState *state, Config *cfg, Key k)
{
    if (dialog_navigate(state, k)) {
        state->dirty = true;
        return;
    }

    switch (k.kind) {
    case KEY_ESCAPE:
        sessions_dialog_close(state);
        break;
    case KEY_ENTER:
    case KEY_NEWLINE: {
        /* treffer nochmal filtern: layout-daten sind nur im draw
         * gueltig (wie bei handle_models) */
        int hits[DIALOG_MATCH_MAX];
        int n = sessions_match(&state->sessions, state->dialog.search, hits,
                               DIALOG_MATCH_MAX);
        if (n == 0 || state->dialog.selected >= n) {
            break;
        }
        const char *id = state->sessions.items[hits[state->dialog.selected]].id;

        /* die gerade offene session: nichts zu tun, nur dialog zu.
         * jede andere: aktuelle bleibt auf der platte liegen, wie
         * sie ist, die gewaehlte wird geoeffnet und ihr transcript
         * in den chat zurueckgespielt. */
        if (!state->session.active || strcmp(state->session.id, id) != 0) {
            session_end(&state->session);
            if (session_open(&state->session, id) == 0) {
                /* prompt-snapshot der session wiederherstellen:
                 * sie soll danach exakt so weitergehen, wie sie
                 * angefangen wurde (NULL = default, "" = aus) */
                free(cfg->system_prompt);
                cfg->system_prompt = (state->session.system_prompt != NULL)
                                         ? dup_str(state->session.system_prompt)
                                         : NULL;
                if (cfg->system_prompt == NULL &&
                    state->session.system_prompt != NULL) {
                    die("out of memory");
                }
                config_persist(cfg);

                chat_clear(&state->chat);
                state->ctx.total_prompt = 0;
                state->ctx.total_completion = 0;
                state->ctx.dropped = 0;
                (void)session_read_transcript(&state->session, &state->chat,
                                              &state->ctx);
                state->worked_ms = state->session.worked_ms;
                /* renderer-frontier: der chat wurde ersetzt, neu
                 * gedruckt wird nur der schwanz */
                draw_content_reset();
            } else {
                if (chat_append(&state->chat, CHAT_ROLE_ERROR,
                                "session nicht lesbar") != 0) {
                    die("out of memory");
                }
            }
        }
        sessions_dialog_close(state);
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
    int rows;
    int cols;
} StreamRedrawCtx;

/* rows/cols im kontext sind die groesse beim turn-beginn. waehrend
 * die ki arbeitet kann das fenster resized werden – der
 * watchdog-tick bemerkt das flag, vermesst das terminal neu,
 * verankert den renderer und aktualisiert den kontext: danach
 * zeichnen alle weiteren redraws/ticks mit den echten werten.
 * rueckgabe: true, wenn neu verankert wurde (der aufrufer sollte
 * sofort voll zeichnen). */
static bool stream_sync_size(StreamRedrawCtx *rc)
{
    if (!rc->state->resized) {
        return false;
    }
    rc->state->resized = 0;
    int rows = rc->rows;
    int cols = rc->cols;
    if (term_size(&rows, &cols) == 0) {
        rc->rows = rows;
        rc->cols = cols;
    }
    dbg("resize (busy): %dx%d", rc->rows, rc->cols);
    draw_reset(rc->rows, true);
    return true;
}

static void stream_redraw(void *ud)
{
    StreamRedrawCtx *rc = ud;
    if (stream_sync_size(rc)) {
        /* nach dem resize den schwanz sofort voll drucken – ein
         * leichter tick wuerde nur die spinner zeichnen und der
         * bildschirm bliebe bis zum naechsten chunk leer */
        draw(rc->rows, rc->cols, rc->state, rc->cfg);
        return;
    }
    draw(rc->rows, rc->cols, rc->state, rc->cfg);
}

/* leichter watchdog-frame: nur die spinner aktualisieren */
static void stream_tick(void *ud)
{
    StreamRedrawCtx *rc = ud;
    if (stream_sync_size(rc)) {
        draw(rc->rows, rc->cols, rc->state, rc->cfg);
        return;
    }
    draw_busy_tick(rc->rows, rc->cols, rc->state, rc->cfg);
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

static void handle_all(AppState *state, Config *cfg, int *rows, int *cols,
                       Key k)
{
    /* kurzform: die chat-eingabe liegt als member im state */
    Input *input = &state->input;
    /* lokale kopie: waehrend eines turns kann das fenster resized
     * werden (stream_sync_size aktualisiert den StreamRedrawCtx,
     * nicht die variablen des main-loops). nach dem turn steht
     * die echte groesse im kontext – der rufenden schleife wird
     * sie per zeiger mitgeteilt */
    int r = *rows;
    int c = *cols;

    if (state->confirm_quit) {
        state->confirm_quit = false;
        state->dirty = true;
    }

    switch (k.kind) {
    case KEY_NEWLINE:
        input_newline(input, r, cmd_list_height(state), state->confirm_quit);
        state->dirty = true;
        break;
    case KEY_ENTER: {
        char word[64];
        char args[512];
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
            config_persist(cfg);
            /* snapshot der offenen session nachziehen: ein resume soll
             * den prompt wiederherstellen, den die session zuletzt
             * hatte, nicht den von ihrem anfang */
            (void)session_prompt_changed(&state->session, cfg->system_prompt);
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
            dbg("cmd: %s", word);
            switch (cmd_lookup(word)) {
            case CMD_CLEAR:
            case CMD_NEW:
                /* /clear und /new tun dasselbe: aktuelle session
                 * hinterlassen wie sie ist, neu anfangen */
                cmd_new(state);
                break;
            case CMD_MODELS:
                cmd_models(state);
                break;
            case CMD_RESUME:
            case CMD_SESSIONS: /* alias: gleicher dialog */
                cmd_resume(state);
                break;
            case CMD_RENAME:
                cmd_rename(state, cfg, args);
                break;
            case CMD_QUIT:
                state->quit = true;
                break;
            case CMD_SETTINGS:
                cmd_settings(state);
                break;
            case CMD_SYSTEM_PROMPT:
                /* /system-prompt: das terminal geht an den editor
                 * und kommt danach hierher zurueck. r/c sind die
                 * zeiger der main-schleife: draw_content_reset
                 * druckt den schwanz, das frame danach sieht die
                 * (vom editor moeglichst andere) terminalgroesse */
                cmd_system_prompt(state, cfg, &r, &c);
                break;
            default: {
                char prefix[64];
                int idx[COMMAND_COUNT];
                cmd_prefix(input, prefix, sizeof(prefix));
                int m = cmd_match(prefix, idx, COMMAND_COUNT);
                if (m > 0) {
                    switch (idx[0]) {
                    case CMD_CLEAR:
                    case CMD_NEW:
                        cmd_new(state);
                        break;
                    case CMD_MODELS:
                        cmd_models(state);
                        break;
                    case CMD_RESUME:
                    case CMD_SESSIONS: /* alias: gleicher dialog */
                        cmd_resume(state);
                        break;
                    case CMD_RENAME:
                        cmd_rename(state, cfg, args);
                        break;
                    case CMD_SETTINGS:
                        cmd_settings(state);
                        break;
                    case CMD_SYSTEM_PROMPT:
                        cmd_system_prompt(state, cfg, &r, &c);
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
                dbg("sende: %.80s", text);
                if (chat_append(&state->chat, CHAT_ROLE_USER, text) != 0) {
                    die("out of memory");
                }
                /* die erste nachricht oeffnet die session: erst jetzt
                 * gibt es eine id, /rename und die statuszeile haben
                 * ab hier etwas in der hand. scheitert das anlegen,
                 * laeuft der chat ohne aufzeichnung weiter – die
                 * unterhaltung ist immer wichtiger als das protokoll. */
                if (!state->session.active) {
                    (void)session_start(&state->session, cfg);
                }
                (void)session_log_user(&state->session, text);
                free(text);
                /* viewport am ANFANG der eigenen nachricht fest-
                 * machen: eine mehrzeilige nachricht darf nie von
                 * oben beschnitten werden ("dem ende folgen" wuerde
                 * sonst die ersten zeilen wegschneiden). der erste
                 * stream-redraw hebt das wieder auf. */

                state->busy = true;
                state->busy_start_ms = mono_ms();
                draw(r, c, state, cfg);
                StreamRedrawCtx rc = {
                    .state = state,
                    .cfg = cfg,
                    .rows = r,
                    .cols = c,
                };
                SendHooks hooks = {
                    .ctx = &rc,
                    .redraw = stream_redraw,
                    .tick = stream_tick,
                };
                (void)send_stream(state, cfg, &hooks);
                state->busy = false;
                /* hat der watchdog ein resize bemerkt, kennt nur der
                 * kontext die neue groesse – sie gehoert auch in die
                 * variablen des main-loops, sonst zeichnet das erste
                 * frame nach dem turn mit der alten breite */
                if (rc.rows != r || rc.cols != c) {
                    r = rc.rows;
                    c = rc.cols;
                    if (state->resized) {
                        state->resized = 0;
                    }
                }
                /* turn vorbei: die arbeitszeit in die gesamt-
                 * buchhaltung und die session schreiben. gescheiterter
                 * turn zaehlt nicht (kein busy_start gesetzt) */
                if (state->busy_start_ms > 0) {
                    state->worked_ms += mono_ms() - state->busy_start_ms;
                    state->busy_start_ms = 0;
                    session_worked_set(&state->session, state->worked_ms);
                }
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
        /* k.ch ist eine utf-8-sequence (umlaute = 2 bytes): das
         * feld frisst byteweise, die zellzaehlung (wrap_step) ist
         * utf-8-faehig – ein umlaut bleibt EINE zelle */
        for (const char *p = k.ch; *p != '\0'; p++) {
            input_char(input, *p);
        }
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
                autocomplete_command(input, idx[0], main_width(c));
            }
        }
        state->dirty =
            true; /* egal ob vervollstaendigt: redraw zeigt den log */
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
        /* dock neu zeichnen. KEIN \x1b[2J: das wuerde den terminal-
         * scrollback zerstoeren, in dem der ganze verlauf lebt */
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
         * so verhalten sich zsh und fish.
         *
         * gezaehlt wird ueber BILDSCHIRMzeilen: steht der cursor in
         * der zweiten haelfte einer umgebrochenen zeile, soll pfeil-
         * hoch sichtbar eine zeile hoch gehen und nicht die history
         * aufrufen. */
        if (input_screen_up(input, input_field_width(c))) {
            state->dirty = true;
            break;
        }
        history_back(state, input);
        break;
    case KEY_DOWN:
        if (input_screen_down(input, input_field_width(c))) {
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
    case KEY_PGUP:
    case KEY_PGDN:
        /* pgup/pgdn: das blaettern uebernimmt das terminal selbst
         * (tmux-history, mausrad) – die app hat keinen viewport
         * mehr, der verlauf lebt im scrollback */
        break;
    }

    *rows = r;
    *cols = c;
}

void handle_key(AppState *state, Config *cfg, int *rows, int *cols)
{
    Key k = key_read();

    /* jede taste ins debug-log: bei einem haenger sieht man hier,
     * ob die tastatur noch ankommt und was die app als letztes
     * bekommen hat */
    dbg("key: kind=%d ch=%s", (int)k.kind, (k.kind == KEY_CHAR) ? k.ch : "");

    if (k.kind == KEY_CTRL_C) {
        handle_ctrl_c(state, cfg);
    } else if (k.kind == KEY_CTRL_Q) {
        state->quit = true;
    } else if (k.kind == KEY_NONE) {
        /* nur resize-interesse */
    } else if (ui_mode(state) == MODE_SETTINGS) {
        handle_settings(state, cfg, k);
    } else if (ui_mode(state) == MODE_THEME) {
        handle_theme(state, cfg, k);
    } else if (ui_mode(state) == MODE_PROMPT) {
        handle_prompt(state, cfg, k);
    } else if (ui_mode(state) == MODE_SESSIONS) {
        handle_sessions(state, cfg, k);
    } else if (ui_mode(state) == MODE_MODELS) {
        handle_models(state, cfg, k);
    } else {
        handle_all(state, cfg, rows, cols, k);
    }
}
