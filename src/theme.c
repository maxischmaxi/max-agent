#include "theme.h"

#include <stdio.h>
#include <string.h>
#include <sys/select.h>
#include <unistd.h>

/* wie lange auf OSC-antworten gewartet wird. terminals antworten
 * lokal meist in <10ms; der fallback greift bei nicht-antwortenden
 * terminals (z.B. dumb terminals, gepipede tests). */
#define THEME_TIMEOUT_MS 250
#define THEME_STEP_MS    50

#define OSC_MAX 64

static unsigned g_fg[3]; /* default-vordergrund */
static unsigned g_bg[3]; /* default-hintergrund */
static bool g_have_fg = false;
static bool g_have_bg = false;

static Theme g_theme = {
    .name = "auto",
    .match = "\x1b[36m", /* cyan: fallback, funktioniert hell+dunkel */
    .reset = "\x1b[39m",
};

void theme_set(const Theme *t)
{
    if (t != NULL) {
        g_theme = *t;
    }
}

const Theme *theme_current(void)
{
    return &g_theme;
}

/* ------------------------------------------------------------------ */
/* OSC-antworten parsen                                                 */
/* ------------------------------------------------------------------ */

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

/* ein rgb-kanal ("1c1c", "ff", "f", ...) parsen und auf 0-255 skalieren */
static bool parse_channel(const char **s, unsigned *out)
{
    unsigned v = 0;
    int digits = 0;
    while (digits < 4) {
        int h = hex_val(**s);
        if (h < 0) {
            break;
        }
        v = (v * 16U) + (unsigned)h;
        digits++;
        (*s)++;
    }
    if (digits == 0) {
        return false;
    }
    unsigned max = (1U << (4 * digits)) - 1U;
    *out = ((v * 255U) + (max / 2U)) / max;
    return true;
}

bool theme_parse_color_reply(const char *osc, unsigned rgb[3])
{
    if (osc == NULL) {
        return false;
    }
    /* prefix "10;" (vg) oder "11;" (hg) */
    if (osc[0] != '1' || (osc[1] != '0' && osc[1] != '1') || osc[2] != ';') {
        return false;
    }
    const char *s = osc + 3;
    if (strncmp(s, "rgb:", 4) == 0) {
        s += 4;
    } else if (strncmp(s, "rgba:", 5) == 0) { /* alpha ignorieren */
        s += 5;
    } else {
        return false;
    }

    unsigned c[4];
    int n = 0;
    while (n < 4) {
        if (!parse_channel(&s, &c[n])) {
            return false;
        }
        n++;
        if (*s == '/') {
            s++;
        } else {
            break;
        }
    }
    if (n < 3) { /* rgb braucht mindestens 3 kanaele */
        return false;
    }
    rgb[0] = c[0];
    rgb[1] = c[1];
    rgb[2] = c[2];
    return true;
}

/* ------------------------------------------------------------------ */
/* eingabe-stream in OSC-antworten zerlegen, rest an leftover            */
/* ------------------------------------------------------------------ */

typedef enum {
    PAR_GROUND,  /* normaler eingabe-byte */
    PAR_ESC,     /* esc gesehen */
    PAR_OSC,     /* innerhalb \x1b]...-antwort */
    PAR_OSC_ESC, /* esc im OSC: ST ('\x1b\\') erwartet */
    PAR_CSI,     /* innerhalb \x1b[...-antwort (z.B. DA1) */
} ParState;

typedef struct {
    ParState state;
    char osc[OSC_MAX];
    size_t osc_len;
    char *leftover;
    size_t cap;
    size_t len;
} Parser;

static void push_leftover(Parser *p, char c)
{
    if (p->len < p->cap) {
        p->leftover[p->len++] = c;
    }
}

static void osc_finish(Parser *p)
{
    p->osc[p->osc_len] = '\0';
    unsigned rgb[3];
    if (theme_parse_color_reply(p->osc, rgb)) {
        if (p->osc[1] == '0') { /* "10;" = default-vordergrund */
            memcpy(g_fg, rgb, sizeof g_fg);
            g_have_fg = true;
        } else { /* "11;" = default-hintergrund */
            memcpy(g_bg, rgb, sizeof g_bg);
            g_have_bg = true;
        }
    }
    p->osc_len = 0;
}

static void feed(Parser *p, char c)
{
    switch (p->state) {
    case PAR_GROUND:
        if (c == 0x1b) {
            p->state = PAR_ESC;
        } else {
            push_leftover(p, c);
        }
        break;
    case PAR_ESC:
        if (c == ']') {
            p->state = PAR_OSC;
            p->osc_len = 0;
        } else if (c == '[') {
            p->state = PAR_CSI; /* z.B. DA1-antwort: ignorieren */
        } else {
            push_leftover(p, 0x1b);
            push_leftover(p, c);
            p->state = PAR_GROUND;
        }
        break;
    case PAR_OSC:
        if (c == 0x07) { /* BEL-terminator */
            osc_finish(p);
            p->state = PAR_GROUND;
        } else if (c == 0x1b) {
            p->state = PAR_OSC_ESC;
        } else if (p->osc_len < sizeof p->osc - 1) {
            p->osc[p->osc_len++] = c;
        }
        break;
    case PAR_OSC_ESC:
        if (c == '\\') { /* ST-terminator */
            osc_finish(p);
            p->state = PAR_GROUND;
        } else {
            p->state = PAR_OSC; /* esc ohne ST: weiter im OSC */
        }
        break;
    case PAR_CSI:
        if ((unsigned char)c >= 0x40 && (unsigned char)c <= 0x7e) {
            p->state = PAR_GROUND; /* final byte: antwort fertig */
        }
        break;
    }
}

/* ------------------------------------------------------------------ */
/* default-theme aus den abgefragten farben ableiten                    */
/* ------------------------------------------------------------------ */

/* gewichtete luminanz 0-255 (rec.609-naeherung) */
static int luminance(const unsigned rgb[3])
{
    return (int)(((rgb[0] * 299U) + (rgb[1] * 587U) + (rgb[2] * 114U)) / 1000U);
}

static void pick_auto(void)
{
    if (!g_have_bg) {
        return; /* keine antwort: fallback-cyan bleibt */
    }
    if (luminance(g_bg) < 128) {
        /* dunkles terminal: helle palette-farbe fuer kontrast */
        g_theme.match = "\x1b[96m"; /* bright cyan */
    } else {
        /* helles terminal: dunkle palette-farbe */
        g_theme.match = "\x1b[36m"; /* cyan */
    }
}

void theme_init(char *leftover, size_t cap, size_t *len)
{
    *len = 0;
    if (leftover == NULL) {
        cap = 0;
    }

    Parser p;
    memset(&p, 0, sizeof p);
    p.leftover = leftover;
    p.cap = cap;

    /* default-farben abfragen (OSC 10 = vg, OSC 11 = hg) */
    fputs("\x1b]10;?\x1b\\", stdout);
    fputs("\x1b]11;?\x1b\\", stdout);
    fflush(stdout);

    int waited = 0;
    while (!(g_have_fg && g_have_bg) && waited < THEME_TIMEOUT_MS) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(STDIN_FILENO, &fds);
        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = (long)THEME_STEP_MS * 1000L;
        int r = select(STDIN_FILENO + 1, &fds, NULL, NULL, &tv);
        waited += THEME_STEP_MS;
        if (r <= 0) {
            continue; /* timeout oder EINTR: weiter warten */
        }
        char b[256];
        ssize_t n = read(STDIN_FILENO, b, sizeof b);
        if (n <= 0) {
            break; /* stdin zu: nichts mehr zu holen */
        }
        for (ssize_t i = 0; i < n; i++) {
            feed(&p, b[i]);
        }
    }

    pick_auto();
    *len = p.len;
}