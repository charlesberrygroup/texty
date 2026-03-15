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
