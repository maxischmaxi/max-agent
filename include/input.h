#ifndef MAX_AGENT_INPUT
#define MAX_AGENT_INPUT

#include <stdbool.h>
#include <stddef.h>
#define INPUT_MAX_LINES 16

typedef struct {
    char *lines[INPUT_MAX_LINES];
    size_t count;
} Input;

const char *last_word(const Input *in);
bool input_in_cmd(const Input *in);
void cmd_prefix(const Input *in, char *out, size_t out_sz);
void input_init(Input *in);
void input_reset(Input *in);
void input_backspace(Input *in);
void input_newline(Input *in, int rows, int list_h, bool g_confirm_quit);
void input_char(Input *in, char c, int cols);
void input_free(Input *in);
int bottom_border_for(int rows, int list_h, bool g_confirm_quit);

#endif
