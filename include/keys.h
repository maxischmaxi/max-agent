#ifndef MAX_AGENT_KEYS
#define MAX_AGENT_KEYS

#include <sys/types.h>

#define SEQ_MAX 32

typedef enum {
    KEY_NONE = 0,
    KEY_CHAR,
    KEY_ENTER,
    KEY_NEWLINE,
    KEY_CTRL_C,
    KEY_CTRL_Q,
    KEY_BACKSPACE,
    KEY_ESCAPE,
} KeyKind;

typedef struct {
    KeyKind kind;
    char ch;
} Key;

Key key_from_escape(const char *seq, ssize_t len);
Key key_from_byte(char c);
Key key_read(void);

/* bytes vor die tastatur-eingabe einreihen (z.B. fruehe eingaben,
 * die waehrend des terminal-query-fensters angekommen sind).
 * der interne puffer laeuft nie ueber: ueberschuss wird verworfen. */
void keys_unread(const char *buf, size_t len);

#endif
