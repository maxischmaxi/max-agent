#include "input.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "utils.h"

/* zeiger auf den anfang des letzten worts in der letzten zeile.
 * bei leerem wort zeigt er auf den string-anfang ('\0'). */
const char *last_word(const Input *in)
{
    const char *last = in->lines[in->count - 1];
    size_t len = strlen(last);
    size_t start = len;
    /* false positive: der analyzer sieht einen 1-byte-calloc-block
     * mit strlen > 0 (unmoeglich) - block ist immer >= strlen+1 gross */
    // NOLINTBEGIN(clang-analyzer-security.ArrayBound)
    while (start > 0 && last[start - 1] != ' ') {
        start--;
    }
    // NOLINTEND(clang-analyzer-security.ArrayBound)
    return &last[start];
}

bool input_in_cmd(const Input *in)
{
    const char *word = last_word(in);
    return word[0] == '/';
}

void cmd_prefix(const Input *in, char *out, size_t out_sz)
{
    out[0] = '\0';
    const char *word = last_word(in);
    if (word[0] != '/' || out_sz == 0) {
        return;
    }
    size_t plen = strlen(word + 1); /* wort ohne fuehrendes '/' */
    if (plen >= out_sz) {
        plen = out_sz - 1;
    }
    memcpy(out, word + 1, plen);
    out[plen] = '\0';
}

void input_free(Input *in)
{
    for (size_t i = 0; i < in->count; i++) {
        free(in->lines[i]);
    }
    in->count = 0;
}

void input_char(Input *in, char c, int cols)
{
    char *last = in->lines[in->count - 1];
    size_t len = strlen(last);
    if (len >= (size_t)(cols - 4)) {
        return;
    }
    char *grown = realloc(last, len + 2);
    if (!grown) {
        die("out of memory");
    }
    in->lines[in->count - 1] = grown;
    grown[len] = c;
    grown[len + 1] = '\0';
}

int bottom_border_for(int rows, int list_h, bool g_confirm_quit)
{
    int b = rows - 1;
    if (list_h > 0) {
        b -= list_h; /* command-liste unter der input-bar */
    }
    if (g_confirm_quit) {
        b -= 2;
    }
    if (b < 2) {
        b = 2;
    }
    return b;
}

void input_newline(Input *in, int rows, int list_h, bool g_confirm_quit)
{
    int bottom_border = bottom_border_for(rows, list_h, g_confirm_quit);
    if (in->count >= INPUT_MAX_LINES) {
        return;
    }
    if (bottom_border - (int)in->count - 1 < 2) {
        return;
    }
    char *empty = calloc(1, 1);
    if (!empty) {
        die("out of memory");
    }
    in->lines[in->count++] = empty;
}

void input_backspace(Input *in)
{
    char *last = in->lines[in->count - 1];
    size_t len = strlen(last);
    if (len > 0) {
        last[len - 1] = '\0';
    } else if (in->count > 1) {
        free(last);
        in->count--;
    }
}

void input_reset(Input *in)
{
    input_free(in);
    input_init(in);
}

void input_init(Input *in)
{
    memset(in, 0, sizeof *in);
    in->lines[0] = calloc(1, 1);
    if (!in->lines[0]) {
        die("out of memory");
    }
    in->count = 1;
}
