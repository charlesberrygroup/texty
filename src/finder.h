/*
 * finder.h — Fuzzy File Finder (pure logic, no ncurses)
 * =============================================================================
 * This module provides the core logic for the fuzzy file finder (Ctrl+P):
 *   - Recursive directory walking to collect file paths
 *   - Fuzzy subsequence matching with scoring
 *   - Filtering and sorting results by relevance
 *
 * The display/UI portion lives in display.c (display_finder_popup).
 * This module has NO knowledge of ncurses — it only produces data.
 * =============================================================================
 */

#ifndef FINDER_H
#define FINDER_H

/* ---- Constants ------------------------------------------------------------ */

/** Maximum number of files the finder will collect from the project tree. */
#define FINDER_MAX_FILES   8192

/** Maximum length of the user's search query. */
#define FINDER_QUERY_MAX   128

/** Maximum number of filtered results to display. */
#define FINDER_MAX_RESULTS 512

/* ---- Data Types ----------------------------------------------------------- */

/*
 * FinderFile — one file entry in the collected project file list.
 *
 * path:    absolute path on disk (used to open the file).
 * display: path relative to the project root (what the user sees and
 *          what fuzzy matching runs against).  Shorter and more readable.
 */
typedef struct {
    char path[1024];     /* absolute path    */
    char display[1024];  /* relative path    */
} FinderFile;

/*
 * FinderResult — a scored match from the fuzzy filter.
 *
 * index: position in the FinderFile array (so we can look up the full path).
 * score: higher is better.  0 means no match.
 */
typedef struct {
    int index;
    int score;
} FinderResult;

/* ---- File collection ------------------------------------------------------ */

/**
 * finder_collect_files — recursively walk a directory and collect file paths.
 *
 * Scans `root` recursively, adding all regular files to `files[]`.
 * Each entry stores the absolute path in `path` and the path relative
 * to `root` in `display`.
 *
 * Skips:
 *   - Hidden files/directories (names starting with '.')
 *   - Common build artifact directories (obj, node_modules, build, etc.)
 *
 * Returns the number of files collected (up to `max_files`).
 * Pure logic — no ncurses dependency.
 */
int finder_collect_files(const char *root, FinderFile *files, int max_files);

/* ---- Fuzzy matching ------------------------------------------------------- */

/**
 * finder_fuzzy_score — score how well `query` matches `text`.
 *
 * Performs a case-insensitive subsequence match: all characters in `query`
 * must appear in `text` in order, but not necessarily adjacent.
 *
 * Returns a positive score if the query matches, 0 if it does not.
 *
 * Scoring bonuses:
 *   +5  per consecutive matched character (rewards tight matches)
 *   +10 match at the very start of the text
 *   +8  match after a separator ('/', '_', '-', '.')
 *   +1  exact case match (lowercase query on lowercase text)
 *
 * Penalties:
 *   -1  per skipped character between matches (penalizes scattered matches)
 *
 * Pure logic — no ncurses dependency.  Exposed for unit testing.
 */
int finder_fuzzy_score(const char *query, const char *text);

/* ---- Filtering ------------------------------------------------------------ */

/**
 * finder_filter — filter files by query, sorted by descending score.
 *
 * Scores every file's `display` path against `query`.  Files with score > 0
 * are collected into `results[]` and sorted highest-score-first.
 *
 * When `query` is empty or NULL, all files are included (score 1).
 *
 * Returns the number of matching results (up to `max_results`).
 */
int finder_filter(const FinderFile *files, int num_files,
                  const char *query,
                  FinderResult *results, int max_results);

#endif /* FINDER_H */
