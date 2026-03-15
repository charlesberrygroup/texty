/*
 * git.h — Git Integration (pure logic, no ncurses)
 * =============================================================================
 * This module detects whether a file is inside a git repository, runs
 * `git diff` to find which lines have changed relative to HEAD, and stores
 * the results so the display module can show colored markers in the gutter.
 *
 * Architecture
 * ------------
 * All git operations shell out to the `git` CLI using popen().  This avoids
 * linking against libgit2 and keeps the build simple.  The trade-off is that
 * each git operation spawns a child process, so we only refresh on file
 * open and save (not every keystroke).
 *
 * The GitState struct is embedded in each Buffer (one per open file).
 * It holds a per-line array of change markers (added/modified/deleted).
 *
 * This module has NO knowledge of ncurses or the display — it only produces
 * data that display.c reads when rendering the gutter.
 * =============================================================================
 */

#ifndef GIT_H
#define GIT_H

/* ---- Line change types ---------------------------------------------------- */

/*
 * GitLineStatus — what happened to a single buffer line relative to HEAD.
 *
 * UNCHANGED:  line is the same as in HEAD (or git info is not available).
 * ADDED:      line is new — it does not exist in HEAD.
 * MODIFIED:   line exists in HEAD but its content changed.
 * DELETED:    one or more lines were deleted AFTER this line.
 *             (We mark the line above the deletion, like VS Code's red arrow.)
 */
typedef enum {
    GIT_LINE_UNCHANGED = 0,
    GIT_LINE_ADDED     = 1,
    GIT_LINE_MODIFIED  = 2,
    GIT_LINE_DELETED   = 3
} GitLineStatus;

/* ---- Per-buffer git state ------------------------------------------------- */

/*
 * GitState — tracks line-level changes for one file.
 *
 * Embedded in each Buffer struct.  Lifecycle:
 *   1. git_state_init() — zeroes everything (called in buffer_create).
 *   2. git_refresh()    — detects repo, runs diff, populates line_status[].
 *   3. git_state_free() — frees heap memory (called in buffer_destroy).
 *
 * When git is not available or the file is not in a repo, line_status is
 * NULL and all lines render without markers.
 */
typedef struct {
    /*
     * line_status — one entry per buffer line (0-based index).
     *
     * Heap-allocated array.  NULL when git info is not available (file is
     * not tracked, not in a repo, or git is not installed).
     *
     * The array is reallocated by git_refresh() to match the buffer's
     * current line count.
     */
    GitLineStatus *line_status;
    int             line_count;     /* number of valid entries */
    int             capacity;       /* allocated slots */

    /*
     * repo_root — absolute path to the git repository root (the directory
     * containing .git/).  Heap-allocated.  NULL if the file is not in a repo.
     */
    char           *repo_root;

    /*
     * is_tracked — 1 if the file is tracked by git, 0 otherwise.
     * Untracked (new) files get no gutter markers.
     */
    int             is_tracked;
} GitState;

/* ---- Functions ------------------------------------------------------------ */

/**
 * git_state_init — zero-initialise a GitState.
 *
 * Call once when a Buffer is created.
 */
void git_state_init(GitState *gs);

/**
 * git_state_free — free all heap memory in a GitState.
 *
 * Call when a Buffer is destroyed.  Safe to call on a zeroed GitState.
 */
void git_state_free(GitState *gs);

/**
 * git_refresh — detect repo and populate line change markers.
 *
 * This is the main entry point.  It:
 *   1. Finds the git repo root (if any) for the given file path.
 *   2. Checks if the file is tracked.
 *   3. Runs `git diff HEAD -- <file>` and parses the unified diff output.
 *   4. Populates gs->line_status[] with one entry per buffer line.
 *
 * `total_lines` is the current number of lines in the buffer — needed to
 * size the line_status array.
 *
 * Returns 0 on success (even if no repo found — line_status will be NULL).
 * Returns -1 on error (out of memory, etc.).
 */
int git_refresh(GitState *gs, const char *filepath, int total_lines);

/**
 * git_parse_diff_output — parse a unified diff string into line markers.
 *
 * This is exposed publicly so it can be tested without running git.
 * `diff_text` is the raw output of `git diff HEAD -- <file>`.
 * `total_lines` is the number of lines in the current (new) file.
 *
 * Populates gs->line_status[] — caller must ensure the array is allocated
 * with at least `total_lines` entries and initialised to GIT_LINE_UNCHANGED.
 *
 * Returns 0 on success, -1 on error.
 */
int git_parse_diff_output(GitState *gs, const char *diff_text, int total_lines);

/* ---- Inline diff structures (for the inline diff view) -------------------- */

/*
 * GitDiffChunk — a group of deleted/changed lines from HEAD.
 *
 * When the user toggles inline diff mode (Ctrl+D), the editor parses the
 * unified diff output into chunks.  Each chunk represents a contiguous block
 * of removed lines from a diff hunk.  A single diff hunk can produce multiple
 * chunks if removals are separated by context lines within the hunk.
 *
 * `before_line` is the 0-based buffer line number in the CURRENT (new) file
 * where the phantom lines should be displayed.  The old lines appear visually
 * ABOVE this buffer line in the inline diff view.
 *
 * For example, if lines were deleted between buffer lines 4 and 5,
 * before_line would be 5 — the phantom lines appear between lines 4 and 5
 * on screen.
 */
typedef struct {
    int     before_line;    /* show old lines before this 0-based buffer line */
    char  **old_lines;      /* heap-allocated array of removed line strings   */
    int     old_count;      /* number of old lines in this chunk              */
} GitDiffChunk;

/*
 * GitDiffChunks — collection of diff chunks for one file.
 *
 * Populated by git_extract_chunks().  Freed by git_diff_chunks_free().
 * Used by the display module to render "phantom" lines (the old content
 * from HEAD) when inline diff mode is active.
 *
 * Chunks are stored in order of increasing before_line, matching the order
 * they appear in the diff output.
 */
typedef struct {
    GitDiffChunk *chunks;   /* heap-allocated array of chunks */
    int           count;    /* number of valid entries        */
    int           capacity; /* allocated slots                */
} GitDiffChunks;

/* ---- Inline diff functions ------------------------------------------------ */

/**
 * git_extract_chunks — parse unified diff text into positioned chunks.
 *
 * Each chunk contains the removed lines from a contiguous deletion or
 * modification within a hunk, along with the 0-based buffer line number
 * they should appear before in the inline diff view.
 *
 * `diff_text` is the raw output of `git diff HEAD -- <file>`.
 *
 * Returns 0 on success, -1 on error.
 */
int git_extract_chunks(GitDiffChunks *dc, const char *diff_text);

/**
 * git_diff_chunks_free — free all memory in a GitDiffChunks.
 *
 * Safe to call on a zeroed or already-freed GitDiffChunks.
 */
void git_diff_chunks_free(GitDiffChunks *dc);

/**
 * git_get_diff_text — run `git diff HEAD -- <file>` and return raw output.
 *
 * Shells out to the `git` CLI.  Returns a heap-allocated string that the
 * caller must free(), or NULL on error.
 *
 * `repo_root` is the absolute path to the git repository root.
 * `filepath`  is the absolute path to the file.
 */
char *git_get_diff_text(const char *repo_root, const char *filepath);

/**
 * git_phantom_lines_in_range — count phantom (old) lines in a line range.
 *
 * Returns the total number of old lines from chunks whose before_line is
 * in [from_line, to_line).  Used by display.c to compute the cursor's
 * screen offset when inline diff is active.
 *
 * For cursor positioning, call with:
 *   from_line = ed->view_row
 *   to_line   = ed->cursor_row + 1
 * so that phantom lines at cursor_row (which appear above the cursor) are
 * included in the offset.
 */
int git_phantom_lines_in_range(const GitDiffChunks *dc,
                               int from_line, int to_line);

/* ---- Hunk staging --------------------------------------------------------- */

/**
 * git_build_hunk_patch — extract a single-hunk patch from a full diff.
 *
 * Finds the hunk in `diff_text` whose new-file line range covers
 * `target_line` (0-based buffer line), and builds a complete patch
 * consisting of the file headers (diff --git, ---, +++) plus that single
 * hunk.  The result can be piped directly to `git apply --cached`.
 *
 * Returns a heap-allocated patch string (caller must free), or NULL if
 * no hunk covers the target line.
 *
 * Exposed publicly so it can be unit-tested without running git.
 */
char *git_build_hunk_patch(const char *diff_text, int target_line);

/**
 * git_stage_hunk_at_line — stage the diff hunk covering a buffer line.
 *
 * Runs `git diff -- <file>` to get UNSTAGED changes, finds the hunk
 * that covers `target_line` (0-based), extracts it as a standalone patch,
 * and applies it to the index with `git apply --cached`.
 *
 * Uses `git diff` (not `git diff HEAD`) so only unstaged changes are
 * staged.  If the line has no unstaged changes, returns -1.
 *
 * Returns 0 on success, -1 on error (no hunk found, apply failed, etc.).
 */
int git_stage_hunk_at_line(const char *repo_root, const char *filepath,
                           int target_line);

/* ---- Committing ----------------------------------------------------------- */

/**
 * git_has_staged_changes — check if there are staged changes in the repo.
 *
 * Runs `git diff --cached --quiet`.  This command exits with code 0 if
 * the index matches HEAD (nothing staged), and code 1 if there are
 * staged changes ready to commit.
 *
 * Returns 1 if there ARE staged changes, 0 if not, -1 on error.
 */
int git_has_staged_changes(const char *repo_root);

/**
 * git_commit — create a commit with the given message.
 *
 * Pipes the message to `git commit -F -` which reads the commit message
 * from stdin.  This avoids shell-escaping issues with quotes and special
 * characters in the message.
 *
 * Returns 0 on success, -1 on error (nothing staged, hook failed, etc.).
 */
int git_commit(const char *repo_root, const char *message);

/* ---- Git blame ------------------------------------------------------------ */

/*
 * GitBlameLine — annotation for a single buffer line.
 *
 * Stores a truncated author name, a short date, and a short commit SHA.
 * Populated by git_parse_blame_output() from `git blame --porcelain`.
 */
typedef struct {
    char author[32];    /* truncated author name, e.g. "John Doe"       */
    char date[11];      /* "YYYY-MM-DD" from author-time epoch          */
    char sha_short[8];  /* first 7 chars of SHA + null terminator       */
} GitBlameLine;

/*
 * GitBlameData — blame annotations for an entire file.
 *
 * Heap-allocated array of GitBlameLine, one per buffer line (0-based).
 * Populated by git_blame_refresh(), freed by git_blame_free().
 */
typedef struct {
    GitBlameLine *lines;    /* array indexed by 0-based buffer line */
    int           count;    /* number of valid entries              */
    int           capacity; /* allocated slots                      */
} GitBlameData;

/**
 * git_parse_blame_output — parse `git blame --porcelain` output.
 *
 * Pure parsing function — no popen, no side effects.  Exposed publicly
 * so it can be unit-tested with known input strings.
 *
 * Porcelain format overview:
 *   - First occurrence of a commit: 40-char SHA + orig_line + final_line + group_count
 *     followed by "author <name>", "author-time <epoch>", etc.
 *   - Subsequent lines from the same commit: 40-char SHA + orig_line + final_line
 *     (no metadata headers — reuse cached info for that SHA).
 *   - Content lines are prefixed with a tab character.
 *
 * `total_lines` is the number of lines in the buffer — used to size the array.
 *
 * Returns 0 on success, -1 on error.
 */
int git_parse_blame_output(GitBlameData *bd, const char *text, int total_lines);

/**
 * git_blame_refresh — run `git blame --porcelain` and populate blame data.
 *
 * Shells out to git and parses the result.  Clears any existing data first.
 * Returns 0 on success, -1 on error.
 */
int git_blame_refresh(GitBlameData *bd, const char *repo_root,
                      const char *filepath, int total_lines);

/**
 * git_blame_free — free all memory in a GitBlameData.
 *
 * Safe to call on a zeroed or already-freed GitBlameData.
 */
void git_blame_free(GitBlameData *bd);

/* ---- Git status list (for the status panel) ------------------------------- */

/** Maximum number of entries in a git status listing. */
#define GIT_STATUS_MAX 512

/*
 * GitStatusEntry — one file from `git status --porcelain=v1`.
 *
 * The porcelain format produces lines like "XY path" where:
 *   X = status in the staging area (index)
 *   Y = status in the working tree
 *
 * For display we combine these into a single user-facing status character.
 */
typedef struct {
    char path[1024];        /* relative path from repo root */
    char index_status;      /* staging area: ' ', M, A, D, R, ? */
    char work_status;       /* working tree: ' ', M, A, D, ? */
} GitStatusEntry;

/*
 * GitStatusList — result of `git status` for a repository.
 *
 * Populated by git_status_refresh().  Freed by git_status_free().
 */
typedef struct {
    GitStatusEntry entries[GIT_STATUS_MAX];
    int            count;
    char           repo_root[1024];   /* cached repo root path */
} GitStatusList;

/**
 * git_status_refresh — run `git status --porcelain=v1` and populate the list.
 *
 * `repo_root` is the path to the git repo root (from GitState.repo_root).
 * Returns 0 on success, -1 on error.
 */
int git_status_refresh(GitStatusList *list, const char *repo_root);

/**
 * git_status_free — reset a GitStatusList to empty.
 *
 * Safe to call on an already-empty list.
 */
void git_status_free(GitStatusList *list);

#endif /* GIT_H */
