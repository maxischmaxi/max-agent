#define _POSIX_C_SOURCE 200809L // NOLINT(bugprone-reserved-identifier)

#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "prompt.h"
#include "test.h"
#include "utils.h"

/* eine verzeichnis-struktur unter tmp anlegen (mkdir_p + write) */
static void mk(const char *dir, const char *file, const char *content)
{
    if (mkdir_p(dir, 0755) != 0) {
        return;
    }
    char path[512];
    (void)snprintf(path, sizeof path, "%s/%s", dir, file);
    (void)write_file(path, content, strlen(content));
}

int main(void)
{
    /* --- hardcoded: kein config-override mehr, immer die vorlage --- */
    char *p = prompt_build();
    /* identitaet + arbeitsregeln wie beim pi-agenten-vorbild */
    CHECK(p != NULL && strstr(p, "You are max agent") != NULL);
    CHECK(p != NULL && strstr(p, "coding agent") != NULL);
    CHECK(p != NULL && strstr(p, "Today's date:") != NULL);
    CHECK(p != NULL && strstr(p, "Working directory:") != NULL);
    CHECK(p != NULL && strstr(p, "same language") != NULL);
    CHECK(p != NULL && strstr(p, "destructive actions") != NULL);
    free(p);

    /* datum im template ist heute (yyyy-mm-dd) */
    char today[16];
    struct tm tmv;
    time_t now = time(NULL);
    CHECK(localtime_r(&now, &tmv) != NULL);
    CHECK(strftime(today, sizeof today, "%Y-%m-%d", &tmv) > 0);

    p = prompt_build();
    CHECK(p != NULL && strstr(p, today) != NULL);
    free(p);

    /* --- effizienz-regeln aus der praxis (71x dasselbe gdb-kommando,
     * ganze dateien wiederholt gelesen): jede runde kostet den
     * vollen kontext, also muss der prompt das modell bremsen --- */
    p = prompt_build();
    CHECK(p != NULL &&
          strstr(p, "Never run the exact same command twice") != NULL);
    CHECK(p != NULL && strstr(p, "failed twice") != NULL);
    CHECK(p != NULL && strstr(p, "offset and limit") != NULL);
    CHECK(p != NULL && strstr(p, "do not re-read a file") != NULL);
    CHECK(p != NULL && strstr(p, "ONE hypothesis") != NULL);
    CHECK(p != NULL && strstr(p, "Batch independent tool calls") != NULL);
    free(p);

    /* zweiter aufruf liefert dieselbe vorlage (kein zustand) */
    char *q = prompt_build();
    char *r = prompt_build();
    CHECK(q != NULL && r != NULL && strcmp(q, r) == 0);
    free(q);
    free(r);

    /* ------------------------------------------------------------------ */
    /* AGENTS.md (wie der pi-agent): global aus dem config-dir im home,   */
    /* dann das projekt-verzeichnis (cwd). HOME und cwd werden fuer die  */
    /* dauer der tests auf wegwerf-verzeichnisse gebogen.                */
    /* ------------------------------------------------------------------ */
    char tmpl_home[] = "/tmp/max-agent-prompt-home-XXXXXX";
    char tmpl_proj[] = "/tmp/max-agent-prompt-proj-XXXXXX";
    char *home = mkdtemp(tmpl_home);
    char *proj = mkdtemp(tmpl_proj);
    CHECK(home != NULL && proj != NULL);
    if (home == NULL || proj == NULL) {
        return test_report();
    }

    char old_cwd[4096];
    CHECK(getcwd(old_cwd, sizeof old_cwd) != NULL);
    const char *old_home = getenv("HOME");
    CHECK(old_home != NULL);

    /* --- 1) keine AGENTS.md: kein <project_context> im prompt --- */
    CHECK(chdir(proj) == 0);
    CHECK(setenv("HOME", home, 1) == 0);
    p = prompt_build();
    CHECK(p != NULL && strstr(p, "<project_context>") == NULL);
    CHECK(p != NULL && strstr(p, "Working directory:") != NULL);
    free(p);

    /* --- 2) global: ~/.config/.maxagent/AGENTS.md, exaktes pi-format,
     * BOM wird gestrippt --- */
    char gdir[512];
    (void)snprintf(gdir, sizeof gdir, "%s/.config/.maxagent", home);
    mk(gdir, "AGENTS.md",
       "\xEF\xBB\xBF# Rules\n- global rule one\n- global rule two");
    p = prompt_build();
    CHECK(p != NULL && strstr(p, "<project_context>") != NULL);
    CHECK(p != NULL && strstr(p, "Project-specific instructions") != NULL);
    CHECK(p != NULL && strstr(p, "global rule one") != NULL);
    CHECK(p != NULL && strstr(p, "\xEF\xBB\xBF") == NULL); /* BOM weg */
    CHECK(p != NULL && strstr(p, "</project_context>") != NULL);
    {
        char expect[700];
        (void)snprintf(expect, sizeof expect,
                       "<project_instructions path=\"%s/AGENTS.md\">\n"
                       "# Rules\n- global rule one\n- global rule two\n"
                       "</project_instructions>",
                       gdir);
        CHECK(p != NULL && strstr(p, expect) != NULL);
    }
    free(p);

    /* --- 3) projekt: <cwd>/AGENTS.md steht HINTER dem globalen block --- */
    mk(proj, "AGENTS.md", "# Project\n- immer tests laufen lassen");
    p = prompt_build();
    CHECK(p != NULL && strstr(p, "immer tests laufen lassen") != NULL);
    {
        const char *g = strstr(p, "global rule one");
        const char *l = strstr(p, "immer tests laufen lassen");
        CHECK(g != NULL && l != NULL && g < l); /* global zuerst */
    }
    free(p);

    /* --- 4) CLAUDE.md faellt zurueck, AGENTS.override.md gewinnt --- */
    /* projekt-AGENTS.md weg, CLAUDE.md rein */
    {
        char path[512];
        (void)snprintf(path, sizeof path, "%s/AGENTS.md", proj);
        CHECK(unlink(path) == 0);
    }
    mk(proj, "CLAUDE.md", "- claude fallback");
    p = prompt_build();
    CHECK(p != NULL && strstr(p, "claude fallback") != NULL);
    free(p);
    mk(proj, "AGENTS.override.md", "- override gewinnt");
    p = prompt_build();
    CHECK(p != NULL && strstr(p, "override gewinnt") != NULL);
    CHECK(p != NULL && strstr(p, "claude fallback") == NULL);
    free(p);
    /* aufraeumen: override + claude weg, AGENTS.md wieder her */
    {
        char path[512];
        (void)snprintf(path, sizeof path, "%s/AGENTS.override.md", proj);
        (void)unlink(path);
        (void)snprintf(path, sizeof path, "%s/CLAUDE.md", proj);
        (void)unlink(path);
    }
    mk(proj, "AGENTS.md", "# Project\n- immer tests laufen lassen");

    /* --- 5) dedup: agent startet im config-dir -> dieselbe datei nur
     * einmal im prompt (pi: seenPaths) --- */
    CHECK(chdir(gdir) == 0);
    p = prompt_build();
    CHECK(p != NULL);
    {
        size_t blocks = 0;
        for (const char *s = p;
             (s = strstr(s, "<project_instructions")) != NULL; s++) {
            blocks++;
        }
        CHECK(blocks == 1);
    }
    free(p);
    CHECK(chdir(proj) == 0);

    /* --- 6) AGENTS.MD (grossbuchstaben) als kandidat dazwischen --- */
    {
        char path[512];
        (void)snprintf(path, sizeof path, "%s/AGENTS.md", proj);
        (void)unlink(path);
    }
    mk(proj, "AGENTS.MD", "- grossbuchstaben variante");
    p = prompt_build();
    CHECK(p != NULL && strstr(p, "grossbuchstaben variante") != NULL);
    free(p);

    /* aufraeumen: HOME/cwd zurueck, wegwerf-verzeichnisse weg */
    CHECK(setenv("HOME", old_home, 1) == 0);
    CHECK(chdir(old_cwd) == 0);
    {
        char path[600];
        (void)snprintf(path, sizeof path, "%s/AGENTS.MD", proj);
        (void)unlink(path);
        (void)snprintf(path, sizeof path, "%s/.config/.maxagent/AGENTS.md",
                       home);
        (void)unlink(path);
        CHECK(rmdir(gdir) == 0);
        (void)snprintf(path, sizeof path, "%s/.config", home);
        CHECK(rmdir(path) == 0);
        CHECK(rmdir(home) == 0);
        CHECK(rmdir(proj) == 0);
    }

    return test_report();
}