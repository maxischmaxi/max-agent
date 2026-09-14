#include "keys.h"

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <sys/select.h>
#include <unistd.h>

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

    int mods = (p[1] > 0) ? p[1] : 1;
    if (final == 'u') {
        if ((p[0] == 13 && (mods == 2 || mods == 3 || mods == 5)) ||
            (p[0] == 106 && mods == 5)) {
            k.kind = KEY_NEWLINE;
        } else if (p[0] == 99 && mods == 5) {
            k.kind = KEY_CTRL_C;
        } else if (p[0] == 113 && mods == 5) {
            k.kind = KEY_CTRL_Q;
        }
    } else if (final == '~') {
        if (p[0] == 27 && p[2] == 13 && (p[1] == 2 || p[1] == 5)) {
            k.kind = KEY_NEWLINE;
        }
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
    } else if (c == 0x03) {
        k.kind = KEY_CTRL_C;
    } else if (c == 0x11) {
        k.kind = KEY_CTRL_Q;
    } else if (c == 0x7f) {
        k.kind = KEY_BACKSPACE;
    } else if (c >= 32 && c <= 126) {
        k.kind = KEY_CHAR;
        k.ch = c;
    }
    return k;
}

Key key_read(void)
{
    if (g_pending_len > 0) {
        if (g_pending[0] == 0x1b) {
            /* esc mitten in einem batch: den rest als sequenz
             * interpretieren, sonst wuerden die folgebytes als
             * normale zeichen durchrutschen */
            Key k = key_from_escape(g_pending, (ssize_t)g_pending_len);
            g_pending_len = 0;
            return k;
        }
        char c = g_pending[0];
        memmove(g_pending, g_pending + 1, g_pending_len - 1);
        g_pending_len--;
        return key_from_byte(c);
    }

    char seq[SEQ_MAX];
    ssize_t n = read(STDIN_FILENO, seq, SEQ_MAX - 1);
    if (n < 0) {
        if (errno == EINTR) {
            return (Key){KEY_NONE, 0}; /* z.B. SIGWINCH */
        }
        die("read failed");
    }
    if (n == 0) { /* EOF: pane zu */
        return (Key){KEY_CTRL_Q, 0};
    }

    if (seq[0] != 0x1b) {
        for (ssize_t i = 1; i < n; i++) {
            g_pending[g_pending_len++] = seq[i];
        }
        return key_from_byte(seq[0]);
    }

    ssize_t len = n;
    while (len < SEQ_MAX - 1) {
        if (!wait_readable(30)) {
            break; /* rest kommt nicht: einzelnes esc */
        }
        char b[8];
        ssize_t m = read(STDIN_FILENO, b, sizeof b);
        if (m <= 0) {
            break;
        }
        for (ssize_t i = 0; i < m && len < SEQ_MAX - 1; i++) {
            seq[len++] = b[i];
        }
        if (len > 0 && seq[len - 1] != 0x1b && is_csi_final(seq[len - 1])) {
            break;
        }
    }
    return key_from_escape(seq, len);
}
