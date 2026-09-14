#ifndef MAX_AGENT_UTILS
#define MAX_AGENT_UTILS

#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>
#include <termios.h>

#include "cJSON.h"

const char *get_home(void);
char *append_to_home(char *suffix);
int read_file(const char *restrict path, char **restrict out_buf,
              size_t *restrict out_size);
int write_file(const char *path, const char *buf, size_t size);
int mkdir_p(const char *path, mode_t mode);
bool is_dir(const char *path);
bool is_file(const char *path);
cJSON *parse_json_file(const char *path);
char *dup_str(const char *s);
void die(const char *msg) __attribute__((noreturn));
void raw_disable(void);
void screen_leave(void);
void restore(void);
void raw_enable(void);
void screen_enter(void);
int term_size(int *rows, int *cols);

#endif
