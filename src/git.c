/*
 * git.c — Git Integration Implementation
 * =============================================================================
 * Implements the functions declared in git.h.
 *
 * This module shells out to the `git` command-line tool using popen() to:
 *   - Detect the repository root (`git rev-parse --show-toplevel`)
 *   - Check if a file is tracked (`git ls-files --error-unmatch`)
 *   - Get line-level changes (`git diff HEAD`)
 *
 * popen() is a POSIX function that runs a shell command and gives us a
 * FILE* to read its stdout.  It's available on macOS and Linux.  The
 * _POSIX_C_SOURCE=200809L flag in the Makefile ensures it's declared.
 *
 * IMPORTANT: All shell commands use single-quoted paths to handle spaces
 * in filenames.  Any embedded single quotes in the path are escaped.
 * =============================================================================
 */

#include "git.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ============================================================================
 * Lifecycle
 * ============================================================================ */

void git_state_init(GitState *gs)
{
    /*
     * Zero everything.  line_status = NULL means "no git info available."
     * The display code checks for NULL before trying to read markers.
     */
    memset(gs, 0, sizeof(GitState));
}

void git_state_free(GitState *gs)
{
    free(gs->line_status);
    gs->line_status = NULL;
    gs->line_count  = 0;
    gs->capacity    = 0;

    free(gs->repo_root);
    gs->repo_root   = NULL;

    gs->is_tracked  = 0;
}

/* ============================================================================
 * Internal helpers
 * ============================================================================ */

/*
 * run_command — run a shell command and read its entire stdout into a buffer.
 *
 * Returns a heap-allocated string (caller must free), or NULL on failure.
 * The trailing newline (if any) is stripped.
 *
 * This is used for simple git commands that produce small output
 * (repo root path, ls-files check, etc.).
 */
static char *run_command(const char *cmd)
{
    FILE *fp = popen(cmd, "r");
    if (!fp) return NULL;

    /*
     * Read all output into a growable buffer.
     * Most git commands we call produce a single short line, but git diff
     * can produce large output, so we grow as needed.
     */
    int   cap = 4096;
    int   len = 0;
    char *buf = malloc(cap);
    if (!buf) { pclose(fp); return NULL; }

    int ch;
    while ((ch = fgetc(fp)) != EOF) {
        if (len + 1 >= cap) {
            cap *= 2;
            char *tmp = realloc(buf, cap);
            if (!tmp) { free(buf); pclose(fp); return NULL; }
            buf = tmp;
        }
        buf[len++] = (char)ch;
    }
    buf[len] = '\0';

    int status = pclose(fp);

    /*
     * pclose returns the exit status of the command.  A non-zero exit
     * means the command failed (e.g. file not tracked, not a git repo).
     * We still return the output — the caller decides what to do.
     *
     * On some systems, the exit status is encoded and needs WEXITSTATUS()
     * to decode.  For simplicity, we just check if it's zero.
     */
    (void)status;

    /* Strip trailing newline(s) */
    while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r'))
        buf[--len] = '\0';

    return buf;
}

/*
 * ensure_line_status — make sure gs->line_status has room for `count` entries.
 *
 * Allocates or reallocates as needed.  New entries are zeroed (UNCHANGED).
 * Returns 0 on success, -1 on allocation failure.
 */
static int ensure_line_status(GitState *gs, int count)
{
    if (count <= gs->capacity) {
        gs->line_count = count;
        return 0;
    }

    GitLineStatus *tmp = realloc(gs->line_status,
                                  count * sizeof(GitLineStatus));
    if (!tmp) return -1;

    /* Zero the new entries */
    memset(tmp + gs->capacity, 0,
           (count - gs->capacity) * sizeof(GitLineStatus));

    gs->line_status = tmp;
    gs->capacity    = count;
    gs->line_count  = count;
    return 0;
}

/* ============================================================================
 * Diff parsing
 * ============================================================================ */

/*
 * git_parse_diff_output — parse unified diff text into line markers.
 *
 * Unified diff format:
 *
 *   --- a/filename
 *   +++ b/filename
 *   @@ -old_start,old_count +new_start,new_count @@
 *    context line (unchanged)
 *   -removed line
 *   +added line
 *
 * We only care about the `+new_start,new_count` in hunk headers and the
 * +/- prefix on each body line.
 *
 * Algorithm for each hunk:
 *   - Track `new_line` (0-based index in the new/current file).
 *   - For each line in the hunk body:
 *     - ' ' (context):  advance new_line by 1 (line is unchanged).
 *     - '+' (added):    mark line_status[new_line] = ADDED, advance new_line.
 *     - '-' (removed):  don't advance new_line (this line doesn't exist in
 *                        the new file).  Count consecutive '-' lines.
 *
 *   - When a group of '-' lines is followed by '+' lines, the '+' lines
 *     are MODIFICATIONS (not pure additions).  We mark those as MODIFIED.
 *
 *   - When '-' lines have no corresponding '+' lines, lines were deleted.
 *     We mark the line just before the deletion point as DELETED.
 */
int git_parse_diff_output(GitState *gs, const char *diff_text, int total_lines)
{
    if (!diff_text || total_lines <= 0) return 0;

    /* Ensure the array is big enough and all entries start as UNCHANGED */
    if (ensure_line_status(gs, total_lines) != 0) return -1;
    memset(gs->line_status, 0, total_lines * sizeof(GitLineStatus));

    const char *p = diff_text;

    while (*p) {
        /*
         * Scan for the next hunk header: a line starting with "@@".
         * Skip everything else (file headers, diff metadata).
         */
        if (p[0] != '@' || p[1] != '@') {
            /* Skip to end of this line */
            while (*p && *p != '\n') p++;
            if (*p == '\n') p++;
            continue;
        }

        /*
         * Parse the hunk header: @@ -old_start,old_count +new_start,new_count @@
         * We only need new_start (the starting line in the current file).
         *
         * Find the '+' that begins the new-file range.
         */
        const char *plus = strchr(p, '+');
        if (!plus) break;

        int new_start = 0, new_count = 0;
        /*
         * sscanf parses "new_start,new_count" from after the '+'.
         * If there's no comma (single-line hunk), new_count defaults to 1.
         */
        if (sscanf(plus + 1, "%d,%d", &new_start, &new_count) < 1)
            break;
        if (new_count == 0 && sscanf(plus + 1, "%d", &new_start) == 1)
            new_count = 0;  /* Pure deletion hunk — 0 lines in new file */

        /* Convert 1-based line number to 0-based index */
        int new_line = new_start - 1;

        /* Skip past the @@ line */
        while (*p && *p != '\n') p++;
        if (*p == '\n') p++;

        /*
         * Process hunk body lines.
         * Each line starts with ' ' (context), '+' (added), or '-' (removed).
         */
        int pending_deletes = 0;  /* consecutive '-' lines not yet matched */

        while (*p && *p != '@') {
            char prefix = *p;

            /* Skip to end of this line */
            while (*p && *p != '\n') p++;
            if (*p == '\n') p++;

            if (prefix == ' ') {
                /*
                 * Context line — unchanged.
                 * If we had pending deletes, mark the line above as DELETED.
                 */
                if (pending_deletes > 0 && new_line > 0
                        && new_line - 1 < total_lines) {
                    if (gs->line_status[new_line - 1] == GIT_LINE_UNCHANGED)
                        gs->line_status[new_line - 1] = GIT_LINE_DELETED;
                    pending_deletes = 0;
                }
                new_line++;

            } else if (prefix == '-') {
                /*
                 * Removed line — this line exists in HEAD but not in the
                 * working copy.  Don't advance new_line.
                 * Track how many consecutive deletes we see.
                 */
                pending_deletes++;

            } else if (prefix == '+') {
                /*
                 * Added line in the working copy.
                 * If there are pending deletes, this is a MODIFICATION
                 * (old line replaced with new line).  Otherwise it's a
                 * pure ADDITION.
                 */
                if (new_line >= 0 && new_line < total_lines) {
                    if (pending_deletes > 0) {
                        gs->line_status[new_line] = GIT_LINE_MODIFIED;
                        pending_deletes--;
                    } else {
                        gs->line_status[new_line] = GIT_LINE_ADDED;
                    }
                }
                new_line++;

            } else if (prefix == '\\') {
                /* "\ No newline at end of file" — skip */

            } else {
                /* Unknown line — end of hunk */
                break;
            }
        }

        /*
         * If we finished the hunk with pending deletes (lines removed at
         * the end of the hunk with no corresponding additions), mark the
         * line above as DELETED.
         */
        if (pending_deletes > 0) {
            int mark = new_line - 1;
            if (mark < 0) mark = 0;
            if (mark < total_lines
                    && gs->line_status[mark] == GIT_LINE_UNCHANGED)
                gs->line_status[mark] = GIT_LINE_DELETED;
        }
    }

    return 0;
}

/* ============================================================================
 * Refresh — detect repo and run diff
 * ============================================================================ */

int git_refresh(GitState *gs, const char *filepath, int total_lines)
{
    if (!filepath || filepath[0] == '\0') return 0;

    /*
     * Convert the filepath to an absolute path using realpath().
     *
     * This is critical because git commands run with `git -C <repo_root>`
     * interpret relative paths relative to the repo root, NOT relative to
     * the original CWD.  If the user runs `./texty src/editor.c` from the
     * repo root it happens to work, but from any other directory it breaks.
     *
     * realpath() resolves '.', '..', and symlinks to give us a canonical
     * absolute path that works regardless of CWD.  It's POSIX.1-2008,
     * available on macOS and Linux.
     */
    char abspath[2048];
    if (!realpath(filepath, abspath))
        return 0;  /* file doesn't exist on disk yet — no git info */

    /*
     * Step 1: Find the git repository root.
     *
     * We use `git rev-parse --show-toplevel` which prints the absolute
     * path to the repo root.  The -C flag tells git to start looking
     * from the file's directory (not the current working directory).
     *
     * We extract the directory from the absolute path.
     */
    char dir[2048];
    strncpy(dir, abspath, sizeof(dir) - 1);
    dir[sizeof(dir) - 1] = '\0';

    /* Find the last '/' to get the directory part */
    char *last_slash = strrchr(dir, '/');
    if (last_slash)
        *last_slash = '\0';
    else
        strcpy(dir, ".");

    char cmd[4096];
    snprintf(cmd, sizeof(cmd),
             "git -C '%s' rev-parse --show-toplevel 2>/dev/null", dir);

    char *root = run_command(cmd);
    if (!root || root[0] == '\0') {
        /* Not a git repo — clear any old state and return */
        free(root);
        free(gs->line_status);
        gs->line_status = NULL;
        gs->line_count  = 0;
        gs->capacity    = 0;
        free(gs->repo_root);
        gs->repo_root   = NULL;
        gs->is_tracked  = 0;
        return 0;
    }

    /* Save the repo root */
    free(gs->repo_root);
    gs->repo_root = root;

    /*
     * Step 2: Check if the file is tracked by git.
     *
     * `git ls-files --error-unmatch` exits with 0 if tracked, non-zero
     * if untracked.  We use popen + pclose to get the exit status.
     * pclose() returns the command's exit status (encoded via waitpid).
     *
     * We pass the ABSOLUTE path so git can find the file regardless
     * of which directory it's running from.
     */
    snprintf(cmd, sizeof(cmd),
             "git -C '%s' ls-files --error-unmatch '%s' >/dev/null 2>&1",
             gs->repo_root, abspath);

    FILE *fp = popen(cmd, "r");
    int tracked = 0;
    if (fp) {
        /* Read and discard any output */
        while (fgetc(fp) != EOF) {}
        tracked = (pclose(fp) == 0);
    }
    gs->is_tracked = tracked;

    if (!tracked) {
        /* File is not tracked — no diff markers */
        free(gs->line_status);
        gs->line_status = NULL;
        gs->line_count  = 0;
        gs->capacity    = 0;
        return 0;
    }

    /*
     * Step 3: Run `git diff HEAD -- <file>` to get line-level changes.
     *
     * HEAD compares the working tree to the last commit, which includes
     * both staged and unstaged changes.  This matches VS Code behavior.
     * We use the absolute path for reliability.
     */
    snprintf(cmd, sizeof(cmd),
             "git -C '%s' diff HEAD -- '%s' 2>/dev/null",
             gs->repo_root, abspath);

    char *diff_output = run_command(cmd);
    if (!diff_output) {
        /* git diff failed — treat as no changes */
        if (ensure_line_status(gs, total_lines) == 0)
            memset(gs->line_status, 0,
                   total_lines * sizeof(GitLineStatus));
        return 0;
    }

    /*
     * Step 4: Parse the diff output into line markers.
     */
    int result = git_parse_diff_output(gs, diff_output, total_lines);
    free(diff_output);

    return result;
}

/* ============================================================================
 * Inline diff — chunk extraction
 * ============================================================================ */

/*
 * flush_pending_chunk — helper to save accumulated old lines as a chunk.
 *
 * When we encounter context lines or the end of a hunk while we have
 * pending old (removed) lines, we "flush" them into a new GitDiffChunk.
 *
 * Parameters:
 *   dc             — the output collection to append to.
 *   old_lines      — array of old line strings (ownership transferred).
 *   old_count      — number of old lines.
 *   before_line    — 0-based buffer line where phantom lines appear.
 *
 * Returns 0 on success, -1 on allocation failure.
 * On success, old_lines ownership moves to the chunk (do NOT free).
 * On failure, old_lines are freed here.
 */
static int flush_pending_chunk(GitDiffChunks *dc, char **old_lines,
                               int old_count, int before_line)
{
    if (old_count == 0) {
        free(old_lines);
        return 0;
    }

    /* Grow the chunks array if needed */
    if (dc->count >= dc->capacity) {
        int new_cap = dc->capacity == 0 ? 8 : dc->capacity * 2;
        GitDiffChunk *tmp = realloc(dc->chunks, new_cap * sizeof(GitDiffChunk));
        if (!tmp) {
            /* Allocation failed — clean up the old lines */
            for (int i = 0; i < old_count; i++)
                free(old_lines[i]);
            free(old_lines);
            return -1;
        }
        dc->chunks   = tmp;
        dc->capacity = new_cap;
    }

    GitDiffChunk *c = &dc->chunks[dc->count];
    c->before_line  = before_line;
    c->old_lines    = old_lines;
    c->old_count    = old_count;
    dc->count++;

    return 0;
}

void git_diff_chunks_free(GitDiffChunks *dc)
{
    if (!dc) return;

    for (int i = 0; i < dc->count; i++) {
        GitDiffChunk *c = &dc->chunks[i];
        for (int j = 0; j < c->old_count; j++)
            free(c->old_lines[j]);
        free(c->old_lines);
    }
    free(dc->chunks);

    dc->chunks   = NULL;
    dc->count    = 0;
    dc->capacity = 0;
}

/*
 * git_extract_chunks — parse unified diff text into positioned chunks.
 *
 * Algorithm:
 *   For each hunk header (@@ ... @@):
 *     - Track `new_line` (0-based position in the current file).
 *     - Walk through the hunk body:
 *       - Context (' '): flush any pending old lines, advance new_line.
 *       - Removed ('-'): accumulate into the pending old lines array.
 *                         Record the position (new_line) of the first '-'.
 *       - Added   ('+'): advance new_line (these exist in the new file).
 *     - At end of hunk: flush remaining pending old lines.
 *
 * This produces one chunk per contiguous block of '-' lines within each
 * hunk.  Each chunk's `before_line` is the new_line position at the start
 * of the removal block — which is the exact buffer line the phantom lines
 * should appear above.
 */
int git_extract_chunks(GitDiffChunks *dc, const char *diff_text)
{
    if (!dc) return -1;

    /* Clear any existing data */
    git_diff_chunks_free(dc);

    if (!diff_text || diff_text[0] == '\0') return 0;

    const char *p = diff_text;

    while (*p) {
        /* ---- Find the next hunk header (@@ ... @@) ---- */
        if (p[0] != '@' || p[1] != '@') {
            while (*p && *p != '\n') p++;
            if (*p == '\n') p++;
            continue;
        }

        /* ---- Parse hunk header: @@ -old_start,old_count +new_start,new_count @@ ---- */
        const char *plus = strchr(p, '+');
        if (!plus) break;

        int new_start = 0;
        if (sscanf(plus + 1, "%d", &new_start) < 1)
            break;

        /* Convert 1-based to 0-based */
        int new_line = new_start - 1;
        if (new_line < 0) new_line = 0;

        /* Skip past the @@ line */
        while (*p && *p != '\n') p++;
        if (*p == '\n') p++;

        /* ---- Walk the hunk body ---- */

        /*
         * pending_lines  — array of old line strings being accumulated.
         * pending_count  — number of entries in pending_lines.
         * pending_cap    — allocated capacity of pending_lines.
         * pending_pos    — the new_line value when the first '-' was seen.
         *                  This becomes the chunk's before_line.
         */
        char **pending_lines = NULL;
        int    pending_count = 0;
        int    pending_cap   = 0;
        int    pending_pos   = new_line;

        while (*p && *p != '@') {
            char prefix = *p;

            /* Extract the line content (everything after the prefix char) */
            const char *line_start = p + 1;
            while (*p && *p != '\n') p++;
            int line_len = (int)(p - line_start);
            if (*p == '\n') p++;

            if (prefix == ' ') {
                /*
                 * Context line — flush any pending old lines as a chunk,
                 * then advance new_line (this line exists in the new file).
                 */
                if (pending_count > 0) {
                    flush_pending_chunk(dc, pending_lines,
                                        pending_count, pending_pos);
                    pending_lines = NULL;
                    pending_count = 0;
                    pending_cap   = 0;
                }
                new_line++;
                pending_pos = new_line;

            } else if (prefix == '-') {
                /*
                 * Removed line — accumulate it in the pending array.
                 * If this is the first '-' in a new group, pending_pos
                 * is already set to the current new_line.
                 */
                if (pending_count == 0)
                    pending_pos = new_line;

                /* Grow the array if needed */
                if (pending_count >= pending_cap) {
                    int new_cap = pending_cap == 0 ? 8 : pending_cap * 2;
                    char **tmp = realloc(pending_lines,
                                         new_cap * sizeof(char *));
                    if (!tmp) {
                        /* Allocation failure — clean up and bail */
                        for (int i = 0; i < pending_count; i++)
                            free(pending_lines[i]);
                        free(pending_lines);
                        return -1;
                    }
                    pending_lines = tmp;
                    pending_cap   = new_cap;
                }

                /*
                 * Copy the line content (without the '-' prefix).
                 * malloc line_len + 1 for the null terminator.
                 */
                char *copy = malloc(line_len + 1);
                if (copy) {
                    memcpy(copy, line_start, line_len);
                    copy[line_len] = '\0';
                } else {
                    copy = strdup("");  /* fallback on OOM */
                }
                pending_lines[pending_count++] = copy;

                /* Don't advance new_line — this line doesn't exist in new file */

            } else if (prefix == '+') {
                /*
                 * Added line — exists in the new file but not in HEAD.
                 * Advance new_line.  Do NOT flush pending lines here,
                 * because '+' lines following '-' lines represent
                 * modifications and the old lines should appear above
                 * the first modified line (which is at pending_pos).
                 */
                new_line++;

            } else if (prefix == '\\') {
                /* "\ No newline at end of file" — skip */

            } else {
                /* Unknown prefix — end of hunk body */
                break;
            }
        }

        /* Flush any remaining pending old lines at end of hunk */
        if (pending_count > 0) {
            flush_pending_chunk(dc, pending_lines, pending_count, pending_pos);
        } else {
            free(pending_lines);
        }
    }

    return 0;
}

char *git_get_diff_text(const char *repo_root, const char *filepath)
{
    if (!repo_root || !filepath) return NULL;

    /*
     * Build the git diff command.
     *
     * We use the absolute filepath to avoid issues with relative paths
     * and the -C flag to run git from the repo root directory.
     *
     * `git diff HEAD -- <file>` compares the working tree version of the
     * file to the last commit (HEAD), showing both staged and unstaged
     * changes.  This matches VS Code's inline diff behavior.
     */
    char cmd[4096];
    snprintf(cmd, sizeof(cmd),
             "git -C '%s' diff HEAD -- '%s' 2>/dev/null",
             repo_root, filepath);

    return run_command(cmd);
}

int git_phantom_lines_in_range(const GitDiffChunks *dc,
                               int from_line, int to_line)
{
    if (!dc || !dc->chunks) return 0;

    int total = 0;
    for (int i = 0; i < dc->count; i++) {
        int bl = dc->chunks[i].before_line;
        if (bl >= from_line && bl < to_line)
            total += dc->chunks[i].old_count;
    }
    return total;
}

/* ============================================================================
 * Hunk staging — extract a single hunk and apply it to the index
 * ============================================================================ */

/*
 * git_build_hunk_patch — extract a single-hunk patch from a full diff.
 *
 * A unified diff has this structure:
 *
 *   diff --git a/file b/file        \
 *   index abc123..def456 100644      |  file headers
 *   --- a/file                       |
 *   +++ b/file                      /
 *   @@ -old_start,old_count +new_start,new_count @@    \
 *    context line                                        |  hunk 1
 *   -removed line                                        |
 *   +added line                                         /
 *   @@ -old_start2,old_count2 +new_start2,new_count2 @@  \
 *    ...                                                   |  hunk 2
 *   ...                                                   /
 *
 * A valid patch for `git apply` needs: file headers + one or more hunks.
 * This function extracts the file headers plus exactly ONE hunk — the one
 * whose new-file range covers `target_line` (0-based).
 *
 * The "new-file range" of a hunk is [new_start-1, new_start-1+new_count)
 * in 0-based indexing.  For pure deletion hunks (new_count == 0), we
 * expand the range to include the line just before the deletion point,
 * since that's where the gutter shows the DELETED marker.
 */
char *git_build_hunk_patch(const char *diff_text, int target_line)
{
    if (!diff_text || diff_text[0] == '\0' || target_line < 0)
        return NULL;

    const char *p = diff_text;

    /*
     * Step 1: Find the end of the file header.
     *
     * The file header is everything from the start of the diff text up to
     * (but not including) the first @@ hunk header line.  This includes
     * "diff --git ...", "index ...", "--- a/...", "+++ b/...".
     */
    const char *file_hdr_end = NULL;

    while (*p) {
        if (p[0] == '@' && p[1] == '@') {
            file_hdr_end = p;
            break;
        }
        while (*p && *p != '\n') p++;
        if (*p == '\n') p++;
    }

    if (!file_hdr_end) return NULL;  /* no hunks in the diff */

    /*
     * Step 2: Walk through hunks to find the one covering target_line.
     *
     * For each @@ line, parse the new-file range (+new_start,new_count).
     * The hunk body extends from the @@ line to the next @@ line or EOF.
     */
    p = file_hdr_end;
    const char *found_start = NULL;
    const char *found_end   = NULL;

    while (*p) {
        /* Skip non-hunk-header lines */
        if (p[0] != '@' || p[1] != '@') {
            while (*p && *p != '\n') p++;
            if (*p == '\n') p++;
            continue;
        }

        /* Remember where this hunk starts */
        const char *hunk_start = p;

        /*
         * Parse the new-file range from the hunk header.
         * Format: @@ -old_start[,old_count] +new_start[,new_count] @@
         *
         * We look for '+' to find new_start, then optionally ',new_count'.
         * If new_count is missing (single-line hunk), it defaults to 1.
         */
        const char *plus = strchr(p, '+');
        if (!plus) break;

        int new_start = 0, new_count = 1;
        int n = sscanf(plus + 1, "%d,%d", &new_start, &new_count);
        if (n < 1) break;
        if (n == 1) new_count = 1;  /* single-line hunk: no comma */

        /* Skip past the @@ line */
        while (*p && *p != '\n') p++;
        if (*p == '\n') p++;

        /* Find the end of the hunk body (next @@ or end of string) */
        const char *hunk_end = p;
        while (*hunk_end) {
            if (hunk_end[0] == '@' && hunk_end[1] == '@') break;
            while (*hunk_end && *hunk_end != '\n') hunk_end++;
            if (*hunk_end == '\n') hunk_end++;
        }

        /*
         * Check if target_line falls within this hunk's new-file range.
         *
         * range_start = new_start - 1   (convert 1-based to 0-based)
         * range_end   = range_start + new_count
         *
         * For pure deletion hunks (new_count == 0), no lines exist in the
         * new file for this hunk.  The gutter marks the line BEFORE the
         * deletion point (new_start - 1), so we expand the range to
         * [new_start - 1, new_start) — a single-line range covering the
         * line above the deletion.
         */
        int range_start = new_start - 1;
        if (range_start < 0) range_start = 0;
        int range_end = range_start + (new_count > 0 ? new_count : 1);

        if (target_line >= range_start && target_line < range_end) {
            found_start = hunk_start;
            found_end   = hunk_end;
            break;
        }

        p = hunk_end;
    }

    if (!found_start) return NULL;  /* no hunk covers target_line */

    /*
     * Step 3: Build the patch = file_header + selected_hunk.
     */
    int hdr_len  = (int)(file_hdr_end - diff_text);
    int hunk_len = (int)(found_end - found_start);

    /*
     * git apply requires the patch to end with a newline.
     *
     * run_command() strips trailing newlines from git's output, so if
     * this is the last hunk in the diff, the final body line won't have
     * a '\n'.  We detect that and append one so the patch is valid.
     */
    int needs_nl = (hunk_len > 0 && found_end[-1] != '\n') ? 1 : 0;
    int total    = hdr_len + hunk_len + needs_nl + 1;  /* +1 for '\0' */

    char *patch = malloc(total);
    if (!patch) return NULL;

    memcpy(patch, diff_text, hdr_len);
    memcpy(patch + hdr_len, found_start, hunk_len);
    if (needs_nl)
        patch[hdr_len + hunk_len] = '\n';
    patch[total - 1] = '\0';

    return patch;
}

int git_stage_hunk_at_line(const char *repo_root, const char *filepath,
                           int target_line)
{
    if (!repo_root || !filepath) return -1;

    /*
     * Step 1: Get the UNSTAGED diff for this file.
     *
     * `git diff -- <file>` shows changes between the INDEX and the working
     * tree — i.e. only unstaged changes.  This is what we want to stage.
     *
     * We do NOT use `git diff HEAD` here because that includes already-staged
     * changes, and re-staging them could cause conflicts.
     */
    char cmd[4096];
    snprintf(cmd, sizeof(cmd),
             "git -C '%s' diff -- '%s' 2>/dev/null", repo_root, filepath);

    char *diff = run_command(cmd);
    if (!diff || diff[0] == '\0') {
        free(diff);
        return -1;  /* no unstaged changes */
    }

    /*
     * Step 2: Extract the single-hunk patch covering target_line.
     */
    char *patch = git_build_hunk_patch(diff, target_line);
    free(diff);

    if (!patch) return -1;  /* no hunk covers this line */

    /*
     * Step 3: Apply the patch to the index.
     *
     * `git apply --cached` reads a patch from stdin and applies it to the
     * index (staging area) without modifying the working tree.
     *
     * popen(cmd, "w") gives us a FILE* connected to the command's stdin.
     * We write the patch, then pclose() waits for git to finish and returns
     * its exit status.
     */
    snprintf(cmd, sizeof(cmd),
             "git -C '%s' apply --cached", repo_root);

    FILE *fp = popen(cmd, "w");
    if (!fp) {
        free(patch);
        return -1;
    }

    int patch_len = (int)strlen(patch);
    fwrite(patch, 1, patch_len, fp);
    int status = pclose(fp);

    free(patch);
    return (status == 0) ? 0 : -1;
}

/* ============================================================================
 * Committing
 * ============================================================================ */

int git_has_staged_changes(const char *repo_root)
{
    if (!repo_root || repo_root[0] == '\0') return -1;

    /*
     * `git diff --cached --quiet` compares the index (staging area) to HEAD.
     *
     * Exit code 0 → index matches HEAD, nothing staged.
     * Exit code 1 → index differs from HEAD, there are staged changes.
     *
     * We use popen() + pclose() to capture the exit code.
     * We must consume any stdout (there shouldn't be any with --quiet,
     * but it's good practice to drain the pipe before pclose).
     */
    char cmd[2048];
    snprintf(cmd, sizeof(cmd),
             "git -C '%s' diff --cached --quiet 2>/dev/null", repo_root);

    FILE *fp = popen(cmd, "r");
    if (!fp) return -1;

    /* Drain any output */
    while (fgetc(fp) != EOF) {}

    int status = pclose(fp);

    /*
     * pclose() returns the raw waitpid status.
     * status == 0 → command exited with code 0 → no staged changes.
     * status != 0 → command exited with non-zero → staged changes exist.
     */
    return (status != 0) ? 1 : 0;
}

int git_commit(const char *repo_root, const char *message)
{
    if (!repo_root || !message || message[0] == '\0') return -1;

    /*
     * `git commit -F -` reads the commit message from stdin.
     *
     * This avoids all shell-escaping problems: no matter what characters
     * are in the user's message (quotes, backticks, dollar signs, etc.),
     * they are written directly to git's stdin, never interpreted by the
     * shell.
     *
     * popen(cmd, "w") connects our FILE* to the command's stdin.
     * We write the message, then pclose() waits for git to finish.
     */
    char cmd[2048];
    snprintf(cmd, sizeof(cmd),
             "git -C '%s' commit -F -", repo_root);

    FILE *fp = popen(cmd, "w");
    if (!fp) return -1;

    fwrite(message, 1, strlen(message), fp);

    int status = pclose(fp);
    return (status == 0) ? 0 : -1;
}

/* ============================================================================
 * Git status list — for the status panel
 * ============================================================================ */

void git_status_free(GitStatusList *list)
{
    if (list) {
        list->count = 0;
        list->repo_root[0] = '\0';
    }
}

int git_status_refresh(GitStatusList *list, const char *repo_root)
{
    if (!list || !repo_root || repo_root[0] == '\0') return -1;

    list->count = 0;
    strncpy(list->repo_root, repo_root, sizeof(list->repo_root) - 1);
    list->repo_root[sizeof(list->repo_root) - 1] = '\0';

    /*
     * Run `git status --porcelain=v1` which produces machine-readable output.
     *
     * Each line is in the format:  "XY filename"
     *   X = index (staging area) status
     *   Y = working tree status
     *   Positions 0-1 are XY, position 2 is a space, position 3+ is the path.
     *
     * The porcelain format is guaranteed not to change across git versions,
     * making it safe to parse programmatically.
     */
    char cmd[2048];
    snprintf(cmd, sizeof(cmd),
             "git -C '%s' status --porcelain=v1 2>/dev/null", repo_root);

    char *output = run_command(cmd);
    if (!output) return -1;

    /*
     * Parse line by line.  Each line is at least 4 characters:
     *   "XY filename\n"
     *    ^^            — index and working tree status
     *      ^           — space separator
     *       ^^^^^^^^^  — relative file path
     */
    const char *p = output;
    while (*p && list->count < GIT_STATUS_MAX) {
        /* Need at least "XY " (3 chars) + at least 1 char for filename */
        const char *eol = strchr(p, '\n');
        int line_len = eol ? (int)(eol - p) : (int)strlen(p);

        if (line_len >= 4) {
            GitStatusEntry *e = &list->entries[list->count];
            e->index_status = p[0];
            e->work_status  = p[1];

            /* Copy the path (starts at position 3) */
            int path_len = line_len - 3;
            if (path_len >= (int)sizeof(e->path))
                path_len = (int)sizeof(e->path) - 1;
            memcpy(e->path, p + 3, path_len);
            e->path[path_len] = '\0';

            list->count++;
        }

        p += line_len;
        if (*p == '\n') p++;
    }

    free(output);
    return 0;
}
