#define _POSIX_C_SOURCE 200809L // NOLINT(bugprone-reserved-identifier)
/* localtime_r, getcwd */

#include "prompt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "utils.h"

/* ------------------------------------------------------------------ */
/* die fest eingebaute vorlage, orientiert am system-prompt des       */
/* pi-agenten: identitaet, aktuelles datum, arbeitsumgebung und ein   */
/* paar arbeitsregeln. datum und verzeichnis werden pro request neu   */
/* eingesetzt – eine session, die ueber mitternacht laeuft, loggt     */
/* automatisch das neue datum.                                       */
/*                                                                    */
/* die "working style"-regeln sind erfahrungswerte aus echten        */
/* sessions: das modell hat sich sonst in identischen debug-loops     */
/* verheddert (dasselbe gdb-kommando 71x) und ganze dateien          */
/* wiederholt gelesen – jede dieser runden kostet den vollen        */
/* kontext nochmal. die regeln halten es davon ab.                   */
/* ------------------------------------------------------------------ */
static const char PROMPT_TEMPLATE[] =
    "You are max agent, a coding agent that lives in the user's\n"
    "terminal. You help write, understand and debug code, run\n"
    "commands and explain their results.\n"
    "\n"
    "Today's date: %s.\n"
    "Working directory: %s.\n"
    "\n"
    "Rules:\n"
    "- Respond in the same language the user writes in.\n"
    "- Be concise and precise. Use Markdown code blocks for code and\n"
    "  file paths in backticks.\n"
    "- Never invent files, paths, command outputs or APIs. If you are\n"
    "  not sure, say so and ask.\n"
    "- Announce destructive actions (deleting files, force operations,\n"
    "  sudo) before performing them and wait for confirmation.\n"
    "\n"
    "Tools:\n"
    "- You can call tools (read_file, edit_file, write_file, bash);\n"
    "  their definitions are provided separately. Use them to inspect\n"
    "  files and run commands instead of guessing, and quote real\n"
    "  outputs.\n"
    "- Batch independent tool calls into one response instead of one\n"
    "  call per response; every round trip costs time.\n"
    "- Use edit_file for targeted changes: each edits[].oldText must\n"
    "  match exactly and be unique in the file, and stay as small as\n"
    "  possible. Use write_file only for new files or when a file\n"
    "  needs a complete rewrite.\n"
    "\n"
    "Working style:\n"
    "- Think before you act: plan the steps of a larger task, then\n"
    "  execute them. Prefer one decisive action over many probing\n"
    "  ones.\n"
    "- Never run the exact same command twice; the output will not\n"
    "  change. If an approach has failed twice, stop repeating it\n"
    "  and change your strategy instead.\n"
    "- Read large files with offset and limit instead of pulling the\n"
    "  whole file, and do not re-read a file you have already seen\n"
    "  in this conversation.\n"
    "- When debugging, form ONE hypothesis, test it with ONE\n"
    "  command, and think about the result before running the next.\n";

/* ------------------------------------------------------------------ */
/* AGENTS.md: projektspezifische anweisungen, exakt wie der pi-agent   */
/* sie laedt und einbettet.                                           */
/*                                                                    */
/* pro verzeichnis gewinnen die kandidaten in dieser reihenfolge – die */
/* erste existierende datei zaehlt, die anderen werden ignoriert:    */
/*   AGENTS.override.md, AGENTS.md, AGENTS.MD, CLAUDE.md, CLAUDE.MD   */
/* (pi: loadContextFileFromDir). ein utf-8-BOM am anfang wird        */
/* gestrippt. geladen werden hoechstens zwei verzeichnisse: das      */
/* config-verzeichnis im home (~/.config/.maxagent, die "globalen"   */
/* regeln) zuerst, dann das projekt-verzeichnis (das cwd beim        */
/* start). pi laedt zusaetzlich alle eltern-verzeichnisse – hier ist  */
/* bewusst nur der projekt-root, wie gewuenscht.                     */
/*                                                                    */
/* die einbettung ins <project_context>-format ist byte-identisch mit */
/* dem pi-agenten: das modell sieht in beiden agents dieselbe struktur. */
/* ------------------------------------------------------------------ */
static const char *const AGENTS_CANDIDATES[] = {
    "AGENTS.override.md", "AGENTS.md", "AGENTS.MD", "CLAUDE.md", "CLAUDE.MD",
};

/* growable string: die vorlage ist fix, aber die AGENTS.md-inhalte
 * sind beliebig gross. die() bei OOM, wie ueberall in der app. */
static void sbuf_append(char **buf, size_t *len, size_t *cap, const char *s,
                        size_t n)
{
    if (*cap < *len + n + 1) {
        size_t cap2 = (*cap > 0) ? *cap : 512;
        while (cap2 < *len + n + 1) {
            cap2 *= 2;
        }
        char *grown = realloc(*buf, cap2);
        if (grown == NULL) {
            die("out of memory");
        }
        *buf = grown;
        *cap = cap2;
    }
    memcpy(*buf + *len, s, n);
    *len += n;
    (*buf)[*len] = '\0';
}

static void sbuf_puts(char **buf, size_t *len, size_t *cap, const char *s)
{
    sbuf_append(buf, len, cap, s, strlen(s));
}

/* die erste vorhandene AGENTS-kandidaten-datei in `dir` lesen.
 * path_out (fuer das path-attribut im prompt) bekommt den vollen
 * pfad der GEFUNDENEN datei. rueckgabe: heap-inhalt (NUL-termi-
 * niert, BOM gestrippt) oder NULL, wenn das verzeichnis keinen
 * der kandidaten enthaelt. */
static char *agents_from_dir(const char *dir, char *path_out, size_t path_sz)
{
    for (size_t i = 0;
         i < sizeof AGENTS_CANDIDATES / sizeof AGENTS_CANDIDATES[0]; i++) {
        if ((size_t)snprintf(path_out, path_sz, "%s/%s", dir,
                             AGENTS_CANDIDATES[i]) >= path_sz) {
            continue; /* pfad zu lang: kandidat ueberspringen */
        }
        if (!is_file(path_out)) {
            continue;
        }
        char *buf = NULL;
        size_t size = 0;
        if (read_file(path_out, &buf, &size) != 0) {
            continue; /* nicht lesbar: naechster kandidat */
        }
        /* utf-8-BOM (EF BB BF) strippen, wie pi (stripBom) */
        char *content = buf;
        size_t len = size;
        if (len >= 3 && (unsigned char)content[0] == 0xEF &&
            (unsigned char)content[1] == 0xBB &&
            (unsigned char)content[2] == 0xBF) {
            memmove(content, content + 3, len - 3);
            len -= 3;
            content[len] = '\0';
        }
        return content;
    }
    return NULL;
}

/* einen <project_instructions>-block im pi-format anhaengen:
 * <project_instructions path="P">\n INHALT \n</project_instructions>\n\n */
static void sbuf_project_block(char **buf, size_t *len, size_t *cap,
                               const char *path, const char *content)
{
    sbuf_puts(buf, len, cap, "<project_instructions path=\"");
    sbuf_puts(buf, len, cap, path);
    sbuf_puts(buf, len, cap, "\">\n");
    sbuf_puts(buf, len, cap, content);
    sbuf_puts(buf, len, cap, "\n</project_instructions>\n\n");
}

char *prompt_build(void)
{
    char cwd[4096];
    if (getcwd(cwd, sizeof cwd) == NULL) {
        snprintf(cwd, sizeof cwd, "unknown");
    }
    char date[16];
    struct tm tmv;
    time_t now = time(NULL);
    if (localtime_r(&now, &tmv) == NULL ||
        strftime(date, sizeof date, "%Y-%m-%d", &tmv) == 0) {
        snprintf(date, sizeof date, "unknown");
    }

    char base[4096];
    int n = snprintf(base, sizeof base, PROMPT_TEMPLATE, date, cwd);
    if (n < 0 || (size_t)n >= sizeof base) {
        return NULL;
    }

    /* AGENTS.md einsammeln: global (config-dir) zuerst, dann projekt-
     * root. dedup: startet der agent direkt im config-dir, ist das
     * dieselbe datei – dann nur einmal (pi: seenPaths) */
    char gpath[4096];
    char ppath[4096];
    char *global_content = NULL;
    char *project_content = NULL;
    char *gdir = append_to_home(".config/.maxagent");
    if (gdir != NULL) {
        global_content = agents_from_dir(gdir, gpath, sizeof gpath);
        free(gdir);
    }
    project_content = agents_from_dir(cwd, ppath, sizeof ppath);
    if (global_content != NULL && project_content != NULL &&
        strcmp(gpath, ppath) == 0) {
        free(project_content);
        project_content = NULL;
    }

    char *out = NULL;
    size_t len = 0;
    size_t cap = 0;
    sbuf_puts(&out, &len, &cap, base);

    if (global_content != NULL || project_content != NULL) {
        /* byte-identisch mit dem pi-agenten (buildSystemPrompt):
         * die struktur ist bei beiden agents dieselbe */
        sbuf_puts(&out, &len, &cap,
                  "\n\n<project_context>\n\n"
                  "Project-specific instructions and guidelines:\n\n");
        if (global_content != NULL) {
            sbuf_project_block(&out, &len, &cap, gpath, global_content);
        }
        if (project_content != NULL) {
            sbuf_project_block(&out, &len, &cap, ppath, project_content);
        }
        sbuf_puts(&out, &len, &cap, "</project_context>\n");
    }

    free(global_content);
    free(project_content);
    return out;
}