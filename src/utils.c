#include "utils.h"

#include <errno.h>
#include <limits.h>
#include <pwd.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

/* terminal-zustand ist privat fuer dieses modul */
static int g_raw = 0;
static int g_alt = 0;
static struct termios g_orig;

const char *get_home(void)
{
    const char *homedir = getenv("HOME");
    if (homedir && homedir[0] != '\0') {
        return homedir;
    }
    struct passwd *pw = getpwuid(getuid());
    if (pw) {
        return pw->pw_dir;
    }
    return NULL;
}

char *append_to_home(char *suffix)
{
    if (suffix == NULL) {
        return NULL;
    }

    const char *sep = suffix[0] == '/' ? "" : "/";

    const char *homedir = get_home();
    if (homedir == NULL) {
        return NULL;
    }

    size_t n = strlen(homedir) + strlen(suffix) + 2;
    char *path = malloc(n);
    if (!path) {
        return NULL;
    }

    snprintf(path, n, "%s%s%s", homedir, sep, suffix);
    return path;
}

int read_file(const char *restrict path, char **restrict out_buf,
              size_t *restrict out_size)
{
    *out_buf = NULL;
    *out_size = 0;

    FILE *f = fopen(path, "r");
    if (f == NULL) {
        return -1;
    }

    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return -1;
    }
    long size = ftell(f);
    if (size < 0 ||
        size > 128L * 1024L * 1024L) { /* 128 MB deckt json-configs locker ab */
        fclose(f);
        return -1;
    }
    if (fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return -1;
    }

    if (size < 0) {
        fclose(f);
        return -1;
    }

    char *data = malloc((size_t)size + 1);
    if (!data) {
        fclose(f);
        return -1;
    }

    size_t cap = (size_t)size;
    size_t nread = fread(data, 1, cap, f);
    if (nread > cap) {
        free(data);
        fclose(f);
        return -1;
    }
    memset(data + nread, 0, 1);
    fclose(f);

    *out_buf = data;
    *out_size = nread;

    return 0;
}

int mkdir_p(const char *path, mode_t mode)
{
    char tmp[4096];
    size_t len = strlen(path);
    if (len >= sizeof(tmp)) {
        return -1;
    }
    memcpy(tmp, path, len + 1);

    for (size_t i = 1; i < len; i++) {
        if (tmp[i] == '/') {
            tmp[i] = '\0';
            if (mkdir(tmp, mode) != 0 && errno != EEXIST) {
                return -1;
            }
            tmp[i] = '/';
        }
    }

    if (mkdir(tmp, mode) != 0 && errno != EEXIST) {
        return -1;
    }
    return 0;
}

bool is_dir(const char *path)
{
    if (!path) {
        return false;
    }
    struct stat st;
    if (stat(path, &st) != 0) {
        return false;
    }
    return S_ISDIR(st.st_mode);
}

bool is_file(const char *path)
{
    if (!path) {
        return false;
    }
    struct stat st;
    if (stat(path, &st) != 0) {
        return false;
    }
    return S_ISREG(st.st_mode);
}

cJSON *parse_json_file(const char *path)
{
    char *buf;
    size_t size;

    int success = read_file(path, &buf, &size);
    if (success != 0) {
        return NULL;
    }

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    return root;
}

char *dup_str(const char *s)
{
    if (!s) {
        return NULL;
    }
    size_t n = strlen(s) + 1;
    char *c = malloc(n);
    if (c) {
        memcpy(c, s, n);
    }
    return c;
}

int write_file(const char *path, const char *buf, size_t size)
{
    FILE *f = fopen(path, "w");
    if (f == NULL) {
        return -1;
    }
    if (fwrite(buf, 1, size, f) != size) {
        fclose(f);
        return -1;
    }
    if (fclose(f) != 0) {
        return -1;
    }
    return 0;
}

void raw_disable(void)
{
    if (g_raw) {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_orig);
    }
    g_raw = 0;
}

void screen_leave(void)
{
    if (g_alt) {
        fputs("\x1b[>4;0m", stdout);
        fputs("\x1b[<u", stdout);
        fputs("\x1b[?25h", stdout);
        fputs("\x1b[?1049l", stdout);
    }
    g_alt = 0;
}

void restore(void)
{
    screen_leave();
    raw_disable();
}

void die(const char *msg)
{
    int saved = errno;
    restore();
    fprintf(stderr, "error: %s (%s)\n", msg, strerror(saved));
    exit(1);
}

void raw_enable(void)
{
    if (tcgetattr(STDIN_FILENO, &g_orig) != 0) {
        die("tcgetattr failed");
    }
    struct termios t = g_orig;
    t.c_iflag &= ~(tcflag_t)(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    t.c_oflag &= ~(tcflag_t)(OPOST);
    t.c_lflag &= ~(tcflag_t)(ECHO | ICANON | IEXTEN | ISIG);
    t.c_cc[VMIN] = 1;
    t.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &t) == 0) {
        g_raw = 1;
    }
}

void screen_enter(void)
{
    fputs("\x1b[?1049h", stdout);
    fputs("\x1b[2J", stdout);
    fputs("\x1b[?25l", stdout);
    fputs("\x1b[>1u", stdout);
    fputs("\x1b[>4;2m", stdout);
    g_alt = 1;
}

int term_size(int *rows, int *cols)
{
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0) {
        *rows = (int)ws.ws_row;
        *cols = (int)ws.ws_col;
        return 0;
    }
    return -1;
}
