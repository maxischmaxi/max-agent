#include "command.h"

#include <string.h>

int cmd_name_col(void)
{
    int w = 0;
    for (int i = 0; i < COMMAND_COUNT; i++) {
        int len = (int)strlen(COMMANDS[i].name);
        if (len > w) {
            w = len;
        }
    }
    return w;
}

int cmd_match(const char *prefix, int *out, int out_max)
{
    int n = 0;
    size_t plen = strlen(prefix);
    for (int i = 0; i < COMMAND_COUNT && n < out_max; i++) {
        if (strncmp(COMMANDS[i].name, prefix, plen) == 0) {
            out[n++] = i;
        }
    }
    return n;
}

int cmd_lookup(const char *word)
{
    if (word == NULL || word[0] == '\0') {
        return -1;
    }
    const char *name = (word[0] == '/') ? word + 1 : word;
    for (int i = 0; i < COMMAND_COUNT; i++) {
        if (strcmp(COMMANDS[i].name, name) == 0) {
            return i;
        }
    }
    return -1;
}
