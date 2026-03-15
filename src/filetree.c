/*
 * filetree.c — File Explorer Tree Implementation
 * =============================================================================
 * See filetree.h for the public API and data-type documentation.
 *
 * KEY C CONCEPTS USED HERE
 * ------------------------
 *
 * opendir / readdir / closedir  (from <dirent.h>)
 *   POSIX functions for reading directory contents.  opendir() opens a
 *   directory and returns an opaque DIR* handle.  readdir() reads one entry
 *   at a time as a struct dirent, which has a d_name field (the basename).
 *   closedir() releases the handle when we are done.
 *
 * stat  (from <sys/stat.h>)
 *   Fills a struct stat with metadata (size, permissions, type) about a file
 *   or directory.  We use S_ISDIR(st.st_mode) to check whether a path is a
 *   directory without relying on dirent's d_type field, which is not portable.
 *
 * qsort  (from <stdlib.h>)
 *   The standard C sort function.  It takes an array, the element count, the
 *   element size, and a comparator function pointer.  The comparator returns
 *   negative/zero/positive to define the ordering (just like strcmp).
 *
 * malloc / free  (from <stdlib.h>)
 *   Heap memory allocation.  malloc(n) returns a void* to n uninitialized
 *   bytes.  free() releases the memory.  Always check malloc's return for NULL.
 *
 * snprintf  (from <stdio.h>)
 *   Like sprintf but with a length limit — prevents buffer overflows.
 *   Always use snprintf, never sprintf, when writing into fixed-size arrays.
 *
 * strncpy / strncmp / strrchr  (from <string.h>)
 *   strncpy(dst, src, n) — copy at most n bytes (always null-terminate manually
 *     if you need to be safe, since strncpy may not add a '\0' when truncating).
 *   strncmp(a, b, n) — compare first n bytes of two strings.
 *   strrchr(s, c) — find the LAST occurrence of character c in string s.
 * =============================================================================
 */

/*
 * _POSIX_C_SOURCE=200809L is already defined in the Makefile's CFLAGS.
 * This exposes POSIX functions like opendir, readdir, stat, etc. that are not
 * part of the basic C99 standard but are available on any POSIX-compliant OS
 * (macOS, Linux, etc.).
 */

#include "filetree.h"

#include <dirent.h>    /* opendir, readdir, closedir, struct dirent */
#include <sys/stat.h>  /* stat, struct stat, S_ISDIR                */
#include <stdlib.h>    /* malloc, free, qsort                        */
#include <string.h>    /* strncpy, strncmp, strcmp, strrchr, strlen  */
#include <stdio.h>     /* snprintf                                   */

/* ============================================================================
 * Internal: temporary entry for sorting within a single directory
 * ============================================================================ */

/*
 * DirItem — used only inside fill_dir() to collect and sort one directory's
 * immediate children before appending them to ft->entries[].
 *
 * We keep it local to this file (not exposed in filetree.h) because it is an
 * implementation detail.  The `static` keyword on the comparator below ensures
 * it is also invisible outside this translation unit.
 */
typedef struct {
    char name[256];   /* basename */
    char path[1024];  /* full path */
    int  is_dir;      /* 1 = directory, 0 = file */
} DirItem;

/*
 * cmp_diritems — comparator for qsort.
 *
 * Sorting rule: directories come before files.  Within the same type (both
 * dirs or both files), sort alphabetically (case-sensitive, strcmp order).
 *
 * HOW qsort COMPARATORS WORK
 * --------------------------
 * qsort calls this function with pointers to two elements of the array.
 * Because qsort uses void*, we cast the pointers to the actual type.
 * Return value convention:
 *   < 0 : a should come before b
 *   = 0 : a and b are equal (order undefined)
 *   > 0 : b should come before a
 */
static int cmp_diritems(const void *a, const void *b)
{
    /*
     * Cast void* to DirItem* so we can access the struct fields.
     * The cast is safe because qsort always passes pointers to elements
     * of the array we gave it, which are all DirItem structs.
     */
    const DirItem *da = (const DirItem *)a;
    const DirItem *db = (const DirItem *)b;

    /* Directories sort before files */
    if (da->is_dir && !db->is_dir) return -1;
    if (!da->is_dir && db->is_dir) return 1;

    /* Same type: alphabetical order */
    return strcmp(da->name, db->name);
}

/* ============================================================================
 * Internal: recursive directory walker
 * ============================================================================ */

/*
 * fill_dir — append entries for `dir_path` to ft->entries[], then recurse
 * into any subdirectory that appears in ft->expanded[].
 *
 * Parameters:
 *   ft       — the FileTree being built (modified in place)
 *   dir_path — absolute path of the directory to scan
 *   depth    — current nesting level (root children start at 0)
 *
 * This function is NOT declared in filetree.h because it is only called
 * internally by filetree_rebuild().  The `static` keyword enforces this.
 */
static void fill_dir(FileTree *ft, const char *dir_path, int depth)
{
    /*
     * Safety guards:
     *   1. Stop if the flat list is already full.
     *   2. Stop if we have recursed unreasonably deep (protects against
     *      circular symlinks or pathologically deep directory trees).
     */
    if (ft->count >= FILETREE_MAX_ENTRIES) return;
    if (depth > 16) return;

    /*
     * opendir() opens the directory stream for reading.
     * If it returns NULL, the directory does not exist or we lack permission.
     * We silently skip it — callers should not see error messages in the UI
     * for directories they cannot read.
     */
    DIR *dir = opendir(dir_path);
    if (!dir) return;

    /*
     * Temporary array to hold this directory's immediate children.
     * We collect all entries first, sort them, then append to ft->entries[].
     *
     * Why not append directly?  Because readdir() does not guarantee any
     * particular order (it depends on the filesystem), so we must collect
     * everything first, sort it, then add it to the flat list.
     *
     * Max 1024 items per directory — sufficient for almost any real project.
     *
     * IMPORTANT: We heap-allocate (malloc) instead of stack-allocating this
     * array.  Each DirItem is ~1284 bytes, so 1024 of them is ~1.25 MB.
     * fill_dir() is recursive (it calls itself for each expanded sub-directory),
     * so a stack array would accumulate ~1.25 MB per nesting level.  With the
     * typical 8 MB stack limit, only 6-7 levels of expansion would cause a
     * stack overflow crash.  Heap allocation avoids this entirely.
     */
    DirItem *items = malloc(1024 * sizeof(DirItem));
    if (!items) {
        closedir(dir);
        return;
    }
    int item_count = 0;

    /*
     * readdir() returns a pointer to a struct dirent describing the next
     * directory entry, or NULL at end-of-directory.  The pointer is valid
     * until the next readdir() call on the same DIR*, so we copy d_name
     * immediately.
     *
     * NOTE: struct dirent also has d_type (DT_DIR, DT_REG, etc.) on many
     * systems, but it is not guaranteed by POSIX and may be DT_UNKNOWN on
     * some filesystems.  We use stat() instead, which is always reliable.
     */
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {

        /* Skip hidden entries: anything starting with a dot.
         * This hides ".", "..", and dot-files like ".git", ".DS_Store". */
        if (ent->d_name[0] == '.') continue;

        /* Protect against overflow of the temporary array */
        if (item_count >= 1024) break;

        /*
         * Build the full path of this entry.
         * snprintf always writes a null terminator within the buffer size,
         * so this is safe even if the strings are very long (just truncated).
         */
        char full_path[1024];
        snprintf(full_path, sizeof(full_path), "%s/%s", dir_path, ent->d_name);

        /*
         * stat() fills a struct stat with information about the file.
         * We use S_ISDIR(st.st_mode) to check if it is a directory.
         * S_ISDIR is a macro defined in <sys/stat.h>.
         *
         * If stat() fails (e.g. a dangling symlink), we skip this entry.
         */
        struct stat st;
        if (stat(full_path, &st) != 0) continue;

        /* Populate the DirItem */
        strncpy(items[item_count].name, ent->d_name,
                sizeof(items[item_count].name) - 1);
        items[item_count].name[sizeof(items[item_count].name) - 1] = '\0';

        strncpy(items[item_count].path, full_path,
                sizeof(items[item_count].path) - 1);
        items[item_count].path[sizeof(items[item_count].path) - 1] = '\0';

        items[item_count].is_dir = S_ISDIR(st.st_mode) ? 1 : 0;

        item_count++;
    }

    /*
     * closedir() releases the directory stream.  Always call this when done,
     * even if we broke out of the loop early.  Forgetting to closedir() leaks
     * a file descriptor.
     */
    closedir(dir);

    /*
     * Sort the collected items: directories first, then files, each group
     * sorted alphabetically.  qsort is an in-place sort — it modifies `items`
     * directly.
     */
    qsort(items, (size_t)item_count, sizeof(DirItem), cmp_diritems);

    /*
     * Append each sorted item to ft->entries[] and recurse into expanded dirs.
     */
    for (int i = 0; i < item_count; i++) {

        /* Check global capacity again inside the loop */
        if (ft->count >= FILETREE_MAX_ENTRIES) break;

        /*
         * Populate the FlatEntry at position ft->count.
         * ft->count++ advances the index AFTER we write to it.
         */
        FlatEntry *e = &ft->entries[ft->count++];

        strncpy(e->name, items[i].name, sizeof(e->name) - 1);
        e->name[sizeof(e->name) - 1] = '\0';

        strncpy(e->path, items[i].path, sizeof(e->path) - 1);
        e->path[sizeof(e->path) - 1] = '\0';

        e->is_dir = items[i].is_dir;
        e->depth  = depth;

        /*
         * If this entry is a directory and it is currently expanded,
         * recurse into it to add its children right below it in the flat list.
         *
         * The children will have depth+1, which makes them render indented.
         *
         * NOTE: items[] has already been freed at the point of recursion?
         * No — free(items) is called AFTER this loop, so items is still valid
         * while we iterate.  The recursive fill_dir() call allocates its OWN
         * items array on the heap; it does not touch our items array.
         */
        if (e->is_dir && filetree_is_expanded(ft, e->path)) {
            fill_dir(ft, e->path, depth + 1);
        }
    }

    /*
     * Release the heap-allocated temporary array.
     * This must be done after the loop because we read items[i] throughout it.
     */
    free(items);
}

/* ============================================================================
 * Public API
 * ============================================================================ */

/*
 * filetree_create — allocate a FileTree and do the initial directory scan.
 *
 * malloc() allocates memory for one FileTree struct on the heap.
 * sizeof(FileTree) is the size in bytes.  We zero the memory with memset
 * so all counts start at 0 and all strings start as empty.
 */
FileTree *filetree_create(const char *root)
{
    /*
     * malloc returns void*, which is implicitly cast to FileTree* in C.
     * Check for NULL — malloc can fail if the system is out of memory.
     */
    FileTree *ft = malloc(sizeof(FileTree));
    if (!ft) return NULL;

    /*
     * memset fills every byte of the struct with 0.
     * This zeroes ft->count, ft->expanded_count, and all string arrays.
     */
    memset(ft, 0, sizeof(FileTree));

    strncpy(ft->root, root, sizeof(ft->root) - 1);
    ft->root[sizeof(ft->root) - 1] = '\0';

    /* Populate entries[] for the first time */
    filetree_rebuild(ft);

    return ft;
}

/*
 * filetree_rebuild — reset entries[] and re-scan from the root directory.
 *
 * This is called both by filetree_create() and by filetree_toggle() after
 * the expanded[] list changes.
 */
void filetree_rebuild(FileTree *ft)
{
    /*
     * Reset the flat list.  We do NOT reset expanded[] — the user's
     * expanded directories persist across rebuilds.
     */
    ft->count = 0;

    /* Recursively populate entries[] starting from depth 0 */
    fill_dir(ft, ft->root, 0);
}

/*
 * filetree_is_expanded — check whether a directory path is in expanded[].
 *
 * We do a linear scan because FILETREE_MAX_EXPANDED (256) is small enough
 * that a binary search would not meaningfully improve performance.
 */
int filetree_is_expanded(const FileTree *ft, const char *path)
{
    for (int i = 0; i < ft->expanded_count; i++) {
        /*
         * strcmp returns 0 when the strings are equal.
         * We use strcmp (not strncmp) because both strings are always
         * null-terminated: expanded[] entries were written with strncpy.
         */
        if (strcmp(ft->expanded[i], path) == 0) return 1;
    }
    return 0;
}

/*
 * filetree_toggle — expand a collapsed directory, or collapse an expanded one.
 *
 * After changing expanded[], we call filetree_rebuild() so the flat list
 * reflects the new state.
 */
void filetree_toggle(FileTree *ft, int idx)
{
    /* Bounds check: idx must point to a valid entry */
    if (idx < 0 || idx >= ft->count) return;

    FlatEntry *e = &ft->entries[idx];

    /* Only directories can be toggled */
    if (!e->is_dir) return;

    if (filetree_is_expanded(ft, e->path)) {
        /*
         * COLLAPSE: remove this path from expanded[].
         *
         * We also remove any nested expanded[] entries whose paths
         * START WITH e->path — otherwise collapsing a parent would leave
         * its children in expanded[], causing them to re-expand immediately
         * when the parent is expanded again.
         *
         * Algorithm: build a new list skipping entries to remove.
         */
        int new_count = 0;
        int path_len  = (int)strlen(e->path);

        for (int i = 0; i < ft->expanded_count; i++) {
            const char *ep = ft->expanded[i];

            /* Remove exact match (the toggled dir itself) */
            if (strcmp(ep, e->path) == 0) continue;

            /*
             * Remove nested paths: if ep starts with "e->path/" then it
             * is a child of the directory being collapsed.
             * strncmp compares the first `path_len` characters.
             * We also check ep[path_len] == '/' to avoid false matches
             * (e.g. "/foo" should not match "/foobar").
             */
            if (strncmp(ep, e->path, (size_t)path_len) == 0
                    && ep[path_len] == '/') {
                continue;
            }

            /* Keep this entry.
             *
             * Only copy when the destination slot differs from the source slot.
             * When new_count == i, both point to the same element — strncpy
             * with identical src and dst is undefined behaviour and traps on
             * macOS's optimised strncpy implementation.
             */
            if (new_count != i) {
                strncpy(ft->expanded[new_count], ep,
                        sizeof(ft->expanded[new_count]) - 1);
                ft->expanded[new_count][sizeof(ft->expanded[new_count]) - 1] = '\0';
            }
            new_count++;
        }
        ft->expanded_count = new_count;

    } else {
        /*
         * EXPAND: add this path to expanded[] (if there is room).
         */
        if (ft->expanded_count < FILETREE_MAX_EXPANDED) {
            strncpy(ft->expanded[ft->expanded_count], e->path,
                    sizeof(ft->expanded[ft->expanded_count]) - 1);
            ft->expanded[ft->expanded_count]
                        [sizeof(ft->expanded[ft->expanded_count]) - 1] = '\0';
            ft->expanded_count++;
        }
    }

    /* Rebuild the flat list to reflect the new expanded[] state */
    filetree_rebuild(ft);
}

/*
 * filetree_free — release heap memory.
 *
 * free(NULL) is defined to be a no-op in C, so it is safe to call even
 * if ft is NULL.
 */
void filetree_free(FileTree *ft)
{
    free(ft);
}

/*
 * filetree_select_path — expand parents of `target` and return its flat index.
 *
 * HOW IT WORKS
 * ------------
 * The flat list only contains entries that are visible (i.e. their parent
 * directory is expanded).  If the target file's parent dirs are collapsed,
 * the entry does not appear in ft->entries[] at all.
 *
 * We fix this by walking every directory component on the path from ft->root
 * to the target file and adding each one to ft->expanded[] if it is not
 * already there.  Then we rebuild the flat list and search for the target.
 *
 * Example:
 *   root   = "/home/user/project"
 *   target = "/home/user/project/src/editor.c"
 *
 *   Intermediate dirs to expand: "/home/user/project/src"
 *   After rebuild, src/editor.c appears in ft->entries[]; we return its index.
 */
int filetree_select_path(FileTree *ft, const char *target)
{
    if (!ft || !target || target[0] == '\0') return -1;

    size_t root_len = strlen(ft->root);

    /*
     * The target must start with ft->root followed by '/'.
     * If it does not, the file is outside our tree and cannot be highlighted.
     */
    if (strncmp(target, ft->root, root_len) != 0) return -1;
    if (target[root_len] != '/') return -1;

    /*
     * Walk through each directory component between root and target.
     *
     * We build `component` one path segment at a time.  When we hit a '/'
     * separator we know `component` is a directory, so we add it to
     * expanded[] if it isn't already there.  We stop before the final
     * component (the file itself — it is not a directory to expand).
     *
     * Example path components for "/home/user/project/src/editor.c":
     *   Pass 1: component = "/home/user/project/src"  → add to expanded[]
     *   Pass 2: "editor.c" has no slash → stop (it is the file)
     */
    char component[1024];
    strncpy(component, ft->root, sizeof(component) - 1);
    component[sizeof(component) - 1] = '\0';

    /* p points to the first character after the root slash */
    const char *p = target + root_len + 1;

    while (*p) {
        const char *slash = strchr(p, '/');
        if (!slash) {
            /* Last component — this is the file itself, not a directory. */
            break;
        }

        /*
         * Append "/" + the directory name to `component`.
         * snprintf always null-terminates within the buffer, so even a very
         * long path just gets silently truncated (which prevents a crash).
         */
        size_t comp_len = strlen(component);
        snprintf(component + comp_len,
                 sizeof(component) - comp_len,
                 "/%.*s", (int)(slash - p), p);

        /* Add to expanded[] if not already there and there is room */
        if (!filetree_is_expanded(ft, component)
                && ft->expanded_count < FILETREE_MAX_EXPANDED) {
            strncpy(ft->expanded[ft->expanded_count], component,
                    sizeof(ft->expanded[ft->expanded_count]) - 1);
            ft->expanded[ft->expanded_count]
                        [sizeof(ft->expanded[ft->expanded_count]) - 1] = '\0';
            ft->expanded_count++;
        }

        p = slash + 1;  /* advance past the '/' to the next component */
    }

    /* Rebuild the flat list now that the parent dirs are expanded */
    filetree_rebuild(ft);

    /* Find the target in the rebuilt flat list */
    for (int i = 0; i < ft->count; i++) {
        if (strcmp(ft->entries[i].path, target) == 0) return i;
    }

    return -1;  /* file not found (e.g. outside root, hidden, or a symlink) */
}
