#define _POSIX_C_SOURCE 200809L // NOLINT(bugprone-reserved-identifier)
/* sigaction, waitpid, WUNTRACED, raise */

#include "editor.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "config.h"
#include "prompt.h"
#include "utils.h"

/* ------------------------------------------------------------------ */
/* effektiver prompt                                                   */
/* ------------------------------------------------------------------ */

char *editor_effective_prompt(const Config *cfg)
{
    return prompt_build(cfg);
}

/* ------------------------------------------------------------------ */
/* editor starten                                                      */
/* ------------------------------------------------------------------ */

/* das kind bekommt dieselbe prozessgruppe wie wir – anders als
 * tool_bash() in tools.c, das eine eigene prozessgruppe anlegt, um
 * alles killen zu koennen. hier soll der editor DAS terminal sein:
 * ctrl+z muss die job-kontrolle der shell erreichen, die kennt
 * uns und das kind nur in einer gruppe. */
int editor_run(const char *path)
{
    const char *ed = getenv("VISUAL");
    if (ed == NULL || ed[0] == '\0') {
        ed = getenv("EDITOR");
    }
    if (ed == NULL || ed[0] == '\0') {
        ed = "vi";
    }

    /* SIGINT ignorieren, BEVOR das kind startet: vim wartet im
     * cooked mode gerne mit "press enter" – ctrl+c dort geht als
     * signal an die ganze prozessgruppe (also auch an uns), und
     * ohne handler waere die app tot. exec setzt ignorierte
     * signale im kind auf default zurueck: der editor bekommt
     * ctrl+c ganz normal. */
    struct sigaction ign;
    memset(&ign, 0, sizeof ign);
    ign.sa_handler = SIG_IGN;
    sigemptyset(&ign.sa_mask);
    struct sigaction old_int;
    (void)sigaction(SIGINT, &ign, &old_int);

    /* git-idiom (prepare_shell_cmd): "sh -c '<editor> "$@"'
     * <editor> <datei>". "$@" umfasst nur die parameter ab $1 –
     * also die datei –, der editor-string selbst steht als $0
     * dahinter und wird von "$@" ausgeschlossen. so funktionieren
     * einfache editoren ("vim") wie editoren mit argumenten
     * ("code -w") und editoren mit metazeichen ("sed -i 's/a/b/'").
     *
     * $EDITOR geht bewusst durch die shell: das ist die definition
     * des features (genau wie git es haelt). der taint-checker
     * meckert zu recht – wir wissen es. */
    size_t ed_len = strlen(ed);
    char *cmd = malloc(ed_len + sizeof " \"$@\"");
    if (cmd == NULL) {
        (void)sigaction(SIGINT, &old_int, NULL);
        return -1;
    }
    (void)snprintf(cmd, ed_len + sizeof " \"$@\"", "%s \"$@\"", ed);

    pid_t pid = fork();
    if (pid < 0) {
        free(cmd);
        (void)sigaction(SIGINT, &old_int, NULL);
        return -1;
    }
    if (pid == 0) {
        /* kind: stdin/stdout/stderr erben – der editor LAEUFT im
         * terminal, hier liegt der ganze sinn. */
        // NOLINTNEXTLINE(clang-analyzer-optin.taint.GenericTaint)
        execl("/bin/sh", "sh", "-c", cmd, ed, path, (char *)NULL);
        _exit(127);
    }
    free(cmd);

    /* eltern: auf das kind warten. ctrl+z im editor stoppt das
     * kind – ohne WUNTRACED wuerde waitpid ewig blockieren. wir
     * reichen das signal an uns selbst weiter, dann kuemmert sich
     * das job-control der shell (der uns gestartet hat) um uns:
     * "fg" holt editor und app gemeinsam zurueck. */
    int status = 0;
    for (;;) {
        pid_t r = waitpid(pid, &status, WUNTRACED);
        if (r == pid) {
            if (WIFSTOPPED(status)) {
                (void)raise(SIGTSTP);
                continue;
            }
            break; /* exit oder kill: fertig */
        }
        if (r < 0 && errno == EINTR) {
            continue;
        }
        break; /* echtes waitpid-versagen: nicht haengen bleiben */
    }

    (void)sigaction(SIGINT, &old_int, NULL);

    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }
    if (WIFSIGNALED(status)) {
        return 128 + WTERMSIG(status);
    }
    return -1;
}