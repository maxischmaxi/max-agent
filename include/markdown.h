#ifndef MAX_AGENT_MARKDOWN
#define MAX_AGENT_MARKDOWN

#include <stdbool.h>
#include <stddef.h>

/* ------------------------------------------------------------------ */
/* markdown: block-scan + zeilen-highlight fuer ki-antworten.        */
/*                                                                    */
/* BLOCKS (fences, tabellen) brauchen zustand ueber zeilen hinweg –   */
/* anders als die zeilenlokalen regeln (headline, listen) laesst sich  */
/* "bin ich in einem ```-block?" nicht aus einer einzelnen zeile     */
/* ablesen. der block-zustand wird deshalb EINMAL pro nachricht in   */
/* md_block_scan() vorausgerechnet: eine MD_BLOCKS-tabelle listet die */
/* byte-offsets der bloecke. draw.c fragt per md_block_at(), ob ein   */
/* offset in einem block liegt.                                       */
/*                                                                    */
/* STREAMING: der text waechst stueckweise. md_block_scan wird je     */
/* frame ueber den GESAMTEN text der nachricht neu ausgefuehrt – die  */
/* tabelle ist damit immer konsistent mit dem, was da ist. ein block  */
/* ohne schliessenden fence bleibt bis zur letzten zeile offen (der   */
/* scanner behandelt das text-ende als schliesser), das highlight     */
/* stabilisiert sich mit jedem chunk. committete zeilen werden NICHT */
/* neu gezeichnet (sie stehen im scrollback) – beim streaming landet  */
/* der block deshalb live in der live-zeile, bis er abgeschlossen     */
/* ist: erst dann committet draw() ihn.                               */
/*                                                                    */
/* CODE-HIGHLIGHT: md_code_token() klassifiziert das zeichen an      */
/* position i im sprachmodus lang: schluesselwort, string, zahl,      */
/* kommentar. die sprachliste ist eine tabelle (keywords je sprache), */
/* unbekannte sprachen highlighten nur strings/kommentare/zahlen.    */
/*                                                                    */
/* TABELLEN: eine markdown-tabelle (kopfzeile, |---|---|, daten) wird  */
/* in einen ausgerichteten string umgebaut – beide spalten so breit  */
/* wie der laengste inhalt, getrennt mit " | ", linksbuendig. die     */
/* tabelle nutzt die GANZE breite des hauptbereichs: ueberschuessige */
/* spalten werden gleichmaessig auf die spalten verteilt. wrap und    */
/* render nutzen beide md_table_display() – der string muss identisch*/
/* sein, sonst brechen layout und druck auseinander (wie bei          */
/* chat_tool_display).                                                */
/* ------------------------------------------------------------------ */

/* zeilenlokale klassifikation (bestehende api, s. u.) */
typedef enum {
    MD_NONE = 0, /* gewoehnliche textzeile */
    MD_HEAD,     /* ueberschrift: # ## ### ... */
    MD_LIST,     /* aufzaehlung: - * + 1. 42. */
    MD_QUOTE,    /* zitat: > */
} MdKind;

typedef struct {
    MdKind kind;       /* was diese zeile ist                         */
    int level;         /* MD_HEAD: anzahl der '#' (1..)               */
    size_t marker;     /* byte-offset des ersten marker-zeichens (>=off) */
    size_t marker_len; /* byte-laenge des markers ("- ", "1.", "### ") */
} MdLine;

void md_scan(const char *text, size_t off, size_t len, bool line_start,
             MdLine *out);

/* ------------------------------------------------------------------ */
/* block-zustand                                                      */
/* ------------------------------------------------------------------ */
typedef enum {
    MD_BLK_NONE = 0,
    MD_BLK_CODE,  /* ``` ... ``` (inhalt wird tokenisiert) */
    MD_BLK_TABLE, /* | a | b | tabelle (layout siehe unten) */
} MdBlockKind;

typedef struct {
    MdBlockKind kind;
    size_t start; /* byte-offset des block-anfangs (inkl. fence)  */
    size_t end;   /* byte-offset NACH dem block-ende (exklusiv)  */
    /* MD_BLK_CODE: sprache aus der fence-zeile ("" = keine). zeigt
     * in den nachricht-text, nicht kopiert – gueltig solange die
     * nachricht existiert. MD_BLK_TABLE: "" */
    const char *lang;
} MdBlock;

#define MD_BLOCKS_MAX 64 /* bloecke je nachricht: genug fuer realen chat */

typedef struct {
    MdBlock blocks[MD_BLOCKS_MAX];
    int n;
} MdBlocks;

/* alle bloecke einer nachricht scannen. text = nachricht (die
 * tabelle verweist per offset hinein, es wird nichts kopiert).
 * wird je frame ausgefuehrt – die tabelle ist ein zwischenergeb-
 * nis, kein cache. */
void md_block_scan(const char *text, MdBlocks *out);

/* liegt off in einem block? NULL, wenn nicht. die zurueckgegebene
 * MdBlock verweist in die tabelle des letzten md_block_scan(). */
const MdBlock *md_block_at(const MdBlocks *blks, size_t off);

/* ------------------------------------------------------------------ */
/* code-tokenisierung                                                  */
/* ------------------------------------------------------------------ */
typedef enum {
    MD_TOK_PLAIN = 0, /* normaler text */
    MD_TOK_KW,        /* schluesselwort der sprache */
    MD_TOK_STR,       /* string-literal ('x' "x") */
    MD_TOK_NUM,       /* zahl */
    MD_TOK_COMMENT,   /* kommentar (raute, doppelslash usw.) */
    MD_TOK_FENCE,     /* die ``` zeile selbst */
} MdTokKind;

typedef struct {
    MdTokKind kind;
    size_t off; /* byte-offset des token-anfangs (relativ zum text) */
    size_t len; /* byte-laenge */
} MdTok;

/* ein token ab position i im text klassifizieren. lang = sprache
 * ("" = generisch). rueckgabe: das token in *tok; die funktion
 * liefert dessen laenge, 0 = i ist am text-ende. die klassen:
 * fence-zeilen bekommen MD_TOK_FENCE komplett, strings/kommentare
 * laufen bis zum schliesser (oder zeilen/text-ende: streaming). */
size_t md_code_token(const char *text, size_t i, const char *lang, MdTok *tok);

/* ------------------------------------------------------------------ */
/* tabellen-layout                                                     */
/* ------------------------------------------------------------------ */
/* ausgerichtete tabelle bauen. text = nachricht, [start,end) = die
 * tabelle (erste zeile = kopf, zweite = trenner, rest = daten).
 * rueckgabe: malloc-ter string (caller freed) oder NULL. die
 * spaltenbreiten folgen dem laengsten inhalt, abstand " | ", die
 * gesamt-breite wird auf 'full_width' verteilt: restplatz gleich-
 * maessig auf die spalten (breite tabelle, wie gewuenscht). */
char *md_table_display(const char *text, size_t start, size_t end,
                       int full_width);

#endif