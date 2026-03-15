/*
 * filetree.h — File Explorer Tree (pure logic, no ncurses)
 * =============================================================================
 * This module manages a "flat list" representation of a directory tree.
 *
 * WHY A FLAT LIST?
 * ----------------
 * A real tree (with parent/child pointers) is complex to render on screen.
 * Instead, we use a flat array of FlatEntry structs — one per visible item.
 * Each entry carries its own `depth` field so we know how far to indent it.
 * When a directory is expanded, its children are inserted right after it in
 * the array.  When collapsed, they are removed.
 *
 * This approach makes rendering trivial: just iterate the array top-to-bottom
 * and draw each entry at column (depth * 2).
 *
 * EXPANDED DIRECTORIES
 * --------------------
 * We track which directories are expanded using a separate array of paths
 * (`expanded[]`).  When we rebuild the tree, we walk the root directory and
 * recurse into any subdirectory whose path appears in `expanded[]`.
 * =============================================================================
 */

#ifndef FILETREE_H
#define FILETREE_H

/* Maximum number of entries visible in the flat list at one time.
 * 2048 is large enough for most projects. */
#define FILETREE_MAX_ENTRIES  2048

/* Maximum number of directories that can be expanded simultaneously.
 * Each expanded directory stores a full path (up to 1024 bytes). */
#define FILETREE_MAX_EXPANDED 256

/* ---- Data Types ----------------------------------------------------------- */

/*
 * FlatEntry — one item visible in the file tree panel.
 *
 * The tree is stored as a flat array of these structs.
 * `depth` controls the visual indentation (depth * 2 spaces).
 *
 * Example for a project with this layout:
 *   src/          depth=0, is_dir=1
 *     main.c      depth=1, is_dir=0
 *     utils.c     depth=1, is_dir=0
 *   README.md     depth=0, is_dir=0
 */
typedef struct {
    char name[256];   /* basename of the entry, e.g. "main.c"              */
    char path[1024];  /* full absolute path, e.g. "/home/user/project/main.c" */
    int  is_dir;      /* 1 if this is a directory, 0 if it is a regular file  */
    int  depth;       /* nesting level; root children are at depth 0          */
} FlatEntry;

/*
 * FileTree — the complete state of the file explorer panel.
 *
 * `root`           — the absolute path of the top-level directory being browsed.
 * `entries[]`      — the flat list of visible items (rebuilt by filetree_rebuild).
 * `count`          — how many entries are currently in the flat list.
 * `expanded[]`     — full paths of directories that are currently expanded.
 * `expanded_count` — how many directories are currently expanded.
 */
typedef struct FileTree {
    char      root[1024];
    FlatEntry entries[FILETREE_MAX_ENTRIES];
    int       count;
    char      expanded[FILETREE_MAX_EXPANDED][1024];
    int       expanded_count;
} FileTree;

/* ---- Functions ------------------------------------------------------------ */

/*
 * filetree_create — allocate and initialise a FileTree rooted at `root`.
 *
 * Calls filetree_rebuild() immediately so `entries` is populated on return.
 * Returns a heap-allocated FileTree (caller must call filetree_free()).
 * Returns NULL on memory allocation failure.
 *
 * C note: malloc() reserves memory on the heap (as opposed to the stack).
 * The caller is responsible for calling free() (via filetree_free()) when done.
 */
FileTree *filetree_create(const char *root);

/*
 * filetree_rebuild — rescan the directory tree and rebuild `ft->entries[]`.
 *
 * Call this whenever files on disk may have changed (e.g. after creating or
 * deleting a file) or when the set of expanded directories changes.
 *
 * The current `expanded[]` list is preserved; directories that are still
 * present on disk remain expanded after the rebuild.
 */
void filetree_rebuild(FileTree *ft);

/*
 * filetree_toggle — expand or collapse the directory at index `idx`.
 *
 * If the entry at `idx` is a directory:
 *   - If it is currently collapsed: add its path to `expanded[]` and rebuild.
 *   - If it is currently expanded:  remove its path from `expanded[]` and rebuild.
 *
 * If `idx` points to a file, this function does nothing.
 */
void filetree_toggle(FileTree *ft, int idx);

/*
 * filetree_is_expanded — return 1 if the directory at `path` is expanded.
 *
 * Returns 0 if the path is not in the `expanded[]` list.
 *
 * C note: `const FileTree *` means we promise not to modify the struct;
 * this is good practice for functions that only need to read state.
 */
int filetree_is_expanded(const FileTree *ft, const char *path);

/*
 * filetree_free — release all memory used by `ft`.
 *
 * After this call, the pointer should not be used.
 */
void filetree_free(FileTree *ft);

/*
 * filetree_select_path — make `target` visible in the tree and return its index.
 *
 * If `target` is inside ft->root, this function expands every intermediate
 * directory on the path from root to the file so the entry becomes visible,
 * then calls filetree_rebuild() and returns the index of the matching entry.
 *
 * Returns the index (>= 0) on success, or -1 if:
 *   - target is NULL or empty
 *   - target is not under ft->root
 *   - target cannot be found even after expanding its parents
 *
 * Used to highlight the currently open file when the tree panel gets focus.
 */
int filetree_select_path(FileTree *ft, const char *target);

#endif /* FILETREE_H */
