/*
 * finder.c — Fuzzy File Finder Implementation
 * =============================================================================
 * Implements the functions declared in finder.h.
 *
 * This module does three things:
 *   1. Walk a directory tree to collect file paths (finder_collect_files)
 *   2. Score query strings against filenames (finder_fuzzy_score)
 *   3. Filter and sort file lists by relevance (finder_filter)
 *
 * All functions are pure logic with no ncurses dependency, so they can
 * be fully unit-tested.
 * =============================================================================
 */

#include "finder.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>       /* for tolower() */
#include <dirent.h>      /* for opendir, readdir, closedir */
#include <sys/stat.h>    /* for stat, S_ISDIR, S_ISREG */

/* ============================================================================
 * File collection — recursive directory walk
 * ============================================================================ */

/*
 * is_skip_dir — check if a directory name should be skipped.
 *
 * We skip hidden directories (starting with '.') and common build artifact
 * directories that would pollute the file list with non-source files.
 */
static int is_skip_dir(const char *name)
{
    /* Hidden directories (.git, .vscode, etc.) */
    if (name[0] == '.') return 1;

    /* Common build artifact / dependency directories */
    static const char *skip_list[] = {
        "obj", "node_modules", "build", "__pycache__",
        "target", "dist", "vendor", ".git",
        NULL
    };

    for (int i = 0; skip_list[i]; i++) {
        if (strcmp(name, skip_list[i]) == 0) return 1;
    }

    return 0;
}

/*
 * collect_recursive — internal recursive walker.
 *
 * `dir`       — absolute path of the current directory to scan.
 * `root_len`  — length of the project root path (for computing relative paths).
 * `files`     — output array.
 * `count`     — pointer to the current number of collected files.
 * `max_files` — capacity of the files array.
 * `depth`     — recursion depth limit (prevents runaway on circular symlinks).
 */
static void collect_recursive(const char *dir, int root_len,
                              FinderFile *files, int *count,
                              int max_files, int depth)
{
    if (depth > 16 || *count >= max_files) return;

    DIR *dp = opendir(dir);
    if (!dp) return;

    struct dirent *ent;
    while ((ent = readdir(dp)) != NULL && *count < max_files) {
        /* Skip . and .. */
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
            continue;

        /* Skip hidden files/directories */
        if (ent->d_name[0] == '.')
            continue;

        /* Build the full path */
        char fullpath[2048];
        snprintf(fullpath, sizeof(fullpath), "%s/%s", dir, ent->d_name);

        /*
         * Use stat() to determine if this is a file or directory.
         * stat() follows symlinks, which is what we want — if someone
         * has a symlink to a source directory, we should traverse it.
         */
        struct stat st;
        if (stat(fullpath, &st) != 0) continue;

        if (S_ISDIR(st.st_mode)) {
            /* Directory — recurse if not in the skip list */
            if (!is_skip_dir(ent->d_name))
                collect_recursive(fullpath, root_len, files, count,
                                  max_files, depth + 1);
        } else if (S_ISREG(st.st_mode)) {
            /* Regular file — add to the list */
            FinderFile *f = &files[*count];

            strncpy(f->path, fullpath, sizeof(f->path) - 1);
            f->path[sizeof(f->path) - 1] = '\0';

            /*
             * The display path is everything after the root directory.
             * root_len includes the root path; we skip it plus the '/' separator.
             * Example: root="/home/user/project", fullpath="/home/user/project/src/main.c"
             *          display = "src/main.c"
             */
            const char *rel = fullpath + root_len;
            if (*rel == '/') rel++;
            strncpy(f->display, rel, sizeof(f->display) - 1);
            f->display[sizeof(f->display) - 1] = '\0';

            (*count)++;
        }
    }

    closedir(dp);
}

int finder_collect_files(const char *root, FinderFile *files, int max_files)
{
    if (!root || !files || max_files <= 0) return 0;

    int count = 0;
    int root_len = (int)strlen(root);

    /* Strip trailing slash if present */
    if (root_len > 1 && root[root_len - 1] == '/')
        root_len--;

    collect_recursive(root, root_len, files, &count, max_files, 0);
    return count;
}

/* ============================================================================
 * Fuzzy matching — subsequence scoring
 * ============================================================================ */

int finder_fuzzy_score(const char *query, const char *text)
{
    if (!query || !text) return 0;

    /* Empty query matches everything with a base score */
    if (query[0] == '\0') return 1;

    int q_len = (int)strlen(query);
    int t_len = (int)strlen(text);

    if (q_len > t_len) return 0;  /* query longer than text — can't match */

    /*
     * Greedy forward scan: try to match each query character against the
     * text, left to right.  Track where each match lands so we can compute
     * scoring bonuses.
     *
     * match_positions[i] = the index in `text` where query[i] matched.
     */
    int match_positions[FINDER_QUERY_MAX];
    int qi = 0;   /* query index */

    for (int ti = 0; ti < t_len && qi < q_len; ti++) {
        if (tolower((unsigned char)query[qi])
                == tolower((unsigned char)text[ti])) {
            match_positions[qi] = ti;
            qi++;
        }
    }

    /* If we didn't match all query characters, no match */
    if (qi < q_len) return 0;

    /*
     * Compute the score based on match quality.
     * Start with a base score of 1 (matched).
     */
    int score = 1;

    for (int i = 0; i < q_len; i++) {
        int pos = match_positions[i];

        /* Bonus: match at the very start of the text */
        if (pos == 0)
            score += 10;

        /* Bonus: match right after a separator character */
        if (pos > 0) {
            char prev = text[pos - 1];
            if (prev == '/' || prev == '_' || prev == '-' || prev == '.')
                score += 8;
        }

        /* Bonus: consecutive match (current position = previous + 1) */
        if (i > 0 && pos == match_positions[i - 1] + 1)
            score += 5;

        /* Bonus: exact case match */
        if (query[i] == text[pos])
            score += 1;

        /* Penalty: skipped characters between this match and the previous */
        if (i > 0) {
            int gap = pos - match_positions[i - 1] - 1;
            if (gap > 0)
                score -= gap;
        }
    }

    /* Slight bonus for shorter display paths (prefer closer matches) */
    score -= t_len / 10;

    /* Ensure minimum score of 1 for any match */
    if (score < 1) score = 1;

    return score;
}

/* ============================================================================
 * Filtering — score all files and sort by relevance
 * ============================================================================ */

/*
 * cmp_results_desc — qsort comparator for FinderResult by descending score.
 */
static int cmp_results_desc(const void *a, const void *b)
{
    const FinderResult *ra = (const FinderResult *)a;
    const FinderResult *rb = (const FinderResult *)b;
    return rb->score - ra->score;  /* higher score first */
}

int finder_filter(const FinderFile *files, int num_files,
                  const char *query,
                  FinderResult *results, int max_results)
{
    if (!files || !results || num_files <= 0 || max_results <= 0)
        return 0;

    int count = 0;

    for (int i = 0; i < num_files && count < max_results; i++) {
        int score = finder_fuzzy_score(query, files[i].display);
        if (score > 0) {
            results[count].index = i;
            results[count].score = score;
            count++;
        }
    }

    /* Sort by descending score */
    if (count > 1)
        qsort(results, count, sizeof(FinderResult), cmp_results_desc);

    return count;
}
