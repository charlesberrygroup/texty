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

/* ============================================================================
 * Symbol extraction — simple pattern matching for go-to-symbol
 * ============================================================================ */

/* Language IDs matching syntax.h SyntaxLang enum.
 * Duplicated here to avoid including syntax.h (keeps finder.c independent). */
#define FINDER_LANG_NONE     0
#define FINDER_LANG_C        1
#define FINDER_LANG_PYTHON   2
#define FINDER_LANG_JS       3
#define FINDER_LANG_RUST     4
#define FINDER_LANG_GO       5

/*
 * skip_ws — return a pointer past leading whitespace in `s`.
 */
static const char *skip_ws(const char *s)
{
    while (*s == ' ' || *s == '\t') s++;
    return s;
}

/*
 * is_ident_char — check if a character can appear in an identifier.
 */
static int is_ident_char(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
        || (c >= '0' && c <= '9') || c == '_';
}

/*
 * extract_word — copy the identifier starting at `p` into `out`.
 * Returns the number of characters consumed.
 */
static int extract_word(const char *p, char *out, int out_size)
{
    int i = 0;
    while (p[i] && is_ident_char(p[i]) && i < out_size - 1) {
        out[i] = p[i];
        i++;
    }
    out[i] = '\0';
    return i;
}

/*
 * add_symbol — helper to add a symbol to the output array if there's room.
 */
static void add_symbol(FinderSymbol *syms, int *count, int max,
                       const char *name, int line, char kind)
{
    if (*count >= max || name[0] == '\0') return;
    FinderSymbol *s = &syms[*count];
    strncpy(s->name, name, sizeof(s->name) - 1);
    s->name[sizeof(s->name) - 1] = '\0';
    s->line = line;
    s->kind = kind;
    (*count)++;
}

/*
 * try_c_function — detect C/C++ function definitions.
 *
 * Heuristic: a line that starts at column 0 (no leading whitespace),
 * contains '(' somewhere, and does NOT start with a control keyword
 * or preprocessor directive.  The function name is the last identifier
 * before the '('.
 *
 * This is imperfect but catches most real-world C function definitions.
 */
static int try_c_function(const char *line, char *name, int name_size)
{
    /* Must start at column 0 (not indented) */
    if (line[0] == ' ' || line[0] == '\t' || line[0] == '\0') return 0;
    if (line[0] == '#' || line[0] == '/' || line[0] == '*') return 0;
    if (line[0] == '{' || line[0] == '}') return 0;

    /* Find the first '(' */
    const char *paren = strchr(line, '(');
    if (!paren || paren == line) return 0;

    /* Skip common non-function lines */
    static const char *skip[] = {
        "if", "for", "while", "switch", "return", "case",
        "else", "do", "sizeof", "typedef", NULL
    };
    for (int i = 0; skip[i]; i++) {
        int slen = (int)strlen(skip[i]);
        if (strncmp(line, skip[i], slen) == 0
                && !is_ident_char(line[slen]))
            return 0;
    }

    /*
     * The function name is the identifier immediately before '('.
     * Walk backward from '(' to find it.
     */
    const char *end = paren;
    while (end > line && *(end - 1) == ' ') end--;  /* skip spaces before '(' */
    if (end <= line) return 0;

    const char *start = end;
    while (start > line && is_ident_char(*(start - 1))) start--;

    if (start == end) return 0;  /* no identifier found */

    int len = (int)(end - start);
    if (len >= name_size) len = name_size - 1;
    memcpy(name, start, len);
    name[len] = '\0';

    return 1;
}

int finder_extract_symbols(const char **lines, int num_lines,
                           int lang,
                           FinderSymbol *symbols, int max_syms)
{
    if (!lines || !symbols || num_lines <= 0 || max_syms <= 0)
        return 0;

    int count = 0;
    char name[128];

    for (int i = 0; i < num_lines && count < max_syms; i++) {
        const char *line = lines[i];
        if (!line || line[0] == '\0') continue;

        const char *trimmed = skip_ws(line);

        /*
         * ---- Language-specific patterns ----
         */

        /* #define NAME (C/C++) */
        if (strncmp(trimmed, "#define ", 8) == 0) {
            extract_word(trimmed + 8, name, sizeof(name));
            add_symbol(symbols, &count, max_syms, name, i + 1, 'd');
            continue;
        }

        /* struct NAME, enum NAME, union NAME, typedef ... NAME (C-like) */
        if (strncmp(trimmed, "struct ", 7) == 0 && is_ident_char(trimmed[7])) {
            extract_word(trimmed + 7, name, sizeof(name));
            add_symbol(symbols, &count, max_syms, name, i + 1, 's');
            continue;
        }
        if (strncmp(trimmed, "enum ", 5) == 0 && is_ident_char(trimmed[5])) {
            extract_word(trimmed + 5, name, sizeof(name));
            add_symbol(symbols, &count, max_syms, name, i + 1, 'e');
            continue;
        }

        /* def name( — Python */
        if (lang == FINDER_LANG_PYTHON
                && strncmp(trimmed, "def ", 4) == 0) {
            extract_word(trimmed + 4, name, sizeof(name));
            add_symbol(symbols, &count, max_syms, name, i + 1, 'f');
            continue;
        }

        /* class name (Python, JS/TS, Rust) */
        if (strncmp(trimmed, "class ", 6) == 0 && is_ident_char(trimmed[6])) {
            extract_word(trimmed + 6, name, sizeof(name));
            add_symbol(symbols, &count, max_syms, name, i + 1, 'c');
            continue;
        }

        /* function name( — JavaScript/TypeScript */
        if ((lang == FINDER_LANG_JS)
                && strncmp(trimmed, "function ", 9) == 0) {
            extract_word(trimmed + 9, name, sizeof(name));
            add_symbol(symbols, &count, max_syms, name, i + 1, 'f');
            continue;
        }

        /* fn name( — Rust */
        if (lang == FINDER_LANG_RUST
                && strncmp(trimmed, "fn ", 3) == 0) {
            extract_word(trimmed + 3, name, sizeof(name));
            add_symbol(symbols, &count, max_syms, name, i + 1, 'f');
            continue;
        }

        /* func name( — Go */
        if (lang == FINDER_LANG_GO
                && strncmp(trimmed, "func ", 5) == 0) {
            const char *p = trimmed + 5;
            /* Skip receiver: func (r *Receiver) Name( */
            if (*p == '(') {
                p = strchr(p, ')');
                if (p) { p++; while (*p == ' ') p++; }
                else continue;
            }
            extract_word(p, name, sizeof(name));
            add_symbol(symbols, &count, max_syms, name, i + 1, 'f');
            continue;
        }

        /* export function/class — JS/TS */
        if (lang == FINDER_LANG_JS && strncmp(trimmed, "export ", 7) == 0) {
            const char *after = trimmed + 7;
            if (strncmp(after, "default ", 8) == 0) after += 8;
            if (strncmp(after, "function ", 9) == 0) {
                extract_word(after + 9, name, sizeof(name));
                add_symbol(symbols, &count, max_syms, name, i + 1, 'f');
                continue;
            }
            if (strncmp(after, "class ", 6) == 0) {
                extract_word(after + 6, name, sizeof(name));
                add_symbol(symbols, &count, max_syms, name, i + 1, 'c');
                continue;
            }
        }

        /*
         * ---- Generic C function detection ----
         * For C/C++ (or unknown languages), try the column-0 heuristic.
         */
        if (lang == FINDER_LANG_C || lang == FINDER_LANG_NONE) {
            if (try_c_function(line, name, sizeof(name)))
                add_symbol(symbols, &count, max_syms, name, i + 1, 'f');
        }
    }

    return count;
}
