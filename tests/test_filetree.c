/*
 * test_filetree.c — Unit tests for filetree.c
 * =============================================================================
 * Tests the file explorer tree logic:
 *   - filetree_create: allocates and populates a tree from a real directory.
 *   - Entry ordering: directories sorted before files, alphabetically within
 *     each group.
 *   - is_dir flag: correctly set for directories and files.
 *   - depth: correctly set for nested entries.
 *   - filetree_toggle: expand a directory (children appear), then collapse
 *     (children disappear).
 *   - filetree_is_expanded: reflects the current expanded[] state.
 *   - filetree_free: no crash on valid pointer or on NULL.
 *
 * HOW THE TEMP DIRECTORY WORKS
 * ----------------------------
 * mkdtemp() creates a unique empty directory under /tmp.
 * On macOS, mkdtemp() is declared in <unistd.h> (not <stdlib.h>).
 * On Linux,  mkdtemp() is declared in <stdlib.h> with _POSIX_C_SOURCE.
 * We include both to be safe.
 *
 * Compile and run via: make test
 * =============================================================================
 */

/*
 * _DARWIN_C_SOURCE enables macOS-specific extensions including mkdtemp().
 * On Linux, _POSIX_C_SOURCE=200809L (already in CFLAGS) is sufficient.
 * Defining _DARWIN_C_SOURCE is harmless on Linux (it is simply ignored).
 */
#define _DARWIN_C_SOURCE

#include "test_runner.h"
#include "filetree.h"

#include <stdio.h>      /* fopen, fclose, snprintf */
#include <stdlib.h>     /* mkdtemp (Linux), free   */
#include <string.h>     /* strcmp, strlen, strncpy */
#include <sys/stat.h>   /* mkdir                   */
#include <unistd.h>     /* mkdtemp (macOS), unlink, rmdir */

/* ============================================================================
 * Helper: create an empty file at `path`
 * ============================================================================ */

/*
 * touch — create an empty file at the given path.
 *
 * fopen(path, "w") opens the file for writing, creating it if it does not
 * exist, truncating to zero bytes if it does.  We immediately close it so
 * the file is just a placeholder.
 *
 * Returns 0 on success, -1 on failure.
 */
static int touch(const char *path)
{
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    fclose(f);
    return 0;
}

/* ============================================================================
 * Helper: build the standard test directory structure
 *
 *   <tmpdir>/
 *     src/             directory (should appear first — dirs before files)
 *       main.c         file
 *       utils.c        file
 *     README.md        file
 *
 * Returns 0 on success, -1 if any creation step fails.
 * ============================================================================ */

static int build_test_tree(const char *tmpdir,
                            char *src_dir,   /* out: path to src/ */
                            char *main_c,    /* out: path to src/main.c */
                            char *utils_c,   /* out: path to src/utils.c */
                            char *readme)    /* out: path to README.md */
{
    /* src/ directory */
    snprintf(src_dir,  1024, "%s/src",          tmpdir);
    snprintf(main_c,   1024, "%s/src/main.c",   tmpdir);
    snprintf(utils_c,  1024, "%s/src/utils.c",  tmpdir);
    snprintf(readme,   1024, "%s/README.md",    tmpdir);

    if (mkdir(src_dir, 0755) != 0)   return -1;
    if (touch(main_c)  != 0)         return -1;
    if (touch(utils_c) != 0)         return -1;
    if (touch(readme)  != 0)         return -1;

    return 0;
}

/* ============================================================================
 * Helper: clean up the test directory structure
 * ============================================================================ */

static void cleanup_test_tree(const char *tmpdir,
                               const char *src_dir,
                               const char *main_c,
                               const char *utils_c,
                               const char *readme)
{
    /*
     * Must remove files before directories (rmdir only works on empty dirs).
     * Order: files first, then subdirectory, then root.
     */
    unlink(main_c);
    unlink(utils_c);
    rmdir(src_dir);
    unlink(readme);
    rmdir(tmpdir);
}

/* ============================================================================
 * Test: filetree_create — basic structure
 * ============================================================================ */

TEST(test_create_basic)
{
    /*
     * mkdtemp() takes a template string ending in "XXXXXX" and replaces
     * the X's with a unique suffix, creating the directory in the process.
     * The template must be a writable buffer (not a string literal), so we
     * use a char array.
     */
    char tmpdir[] = "/tmp/texty_XXXXXX";
    char *result  = mkdtemp(tmpdir);
    ASSERT(result != NULL, "mkdtemp created temp dir");
    if (!result) return;

    char src_dir[1024], main_c[1024], utils_c[1024], readme[1024];
    int ok = build_test_tree(tmpdir, src_dir, main_c, utils_c, readme);
    ASSERT(ok == 0, "build_test_tree succeeded");
    if (ok != 0) { rmdir(tmpdir); return; }

    FileTree *ft = filetree_create(tmpdir);
    ASSERT(ft != NULL, "filetree_create returned non-NULL");

    if (ft) {
        /*
         * The root has two direct children: "src" (dir) and "README.md" (file).
         * Dirs come first, so ft->entries[0] should be src.
         * ft->entries[1] should be README.md.
         * count = 2 (src/ and README.md; src/ is collapsed so no children yet).
         */
        ASSERT(ft->count == 2, "root has 2 visible entries (collapsed)");

        /* First entry: src directory */
        ASSERT(strcmp(ft->entries[0].name, "src")  == 0, "entry[0] is 'src'");
        ASSERT(ft->entries[0].is_dir == 1,               "entry[0] is a dir");
        ASSERT(ft->entries[0].depth  == 0,               "entry[0] depth is 0");

        /* Second entry: README.md file */
        ASSERT(strcmp(ft->entries[1].name, "README.md") == 0, "entry[1] is 'README.md'");
        ASSERT(ft->entries[1].is_dir == 0,               "entry[1] is a file");
        ASSERT(ft->entries[1].depth  == 0,               "entry[1] depth is 0");

        filetree_free(ft);
    }

    cleanup_test_tree(tmpdir, src_dir, main_c, utils_c, readme);
}

/* ============================================================================
 * Test: sort order — directories before files, alphabetical within groups
 * ============================================================================ */

TEST(test_sort_order)
{
    /*
     * Create a structure with multiple dirs and files to verify sorting:
     *
     *   <tmpdir>/
     *     zdir/       directory (alphabetically last, but dirs sort first)
     *     adir/       directory (alphabetically first)
     *     zebra.txt   file (alphabetically last)
     *     apple.txt   file (alphabetically first)
     *
     * Expected order: adir, zdir, apple.txt, zebra.txt
     */
    char tmpdir[] = "/tmp/texty_XXXXXX";
    char *result  = mkdtemp(tmpdir);
    ASSERT(result != NULL, "mkdtemp created temp dir");
    if (!result) return;

    char zdir[1024], adir[1024], zebra[1024], apple[1024];
    snprintf(zdir,  sizeof(zdir),  "%s/zdir",      tmpdir);
    snprintf(adir,  sizeof(adir),  "%s/adir",      tmpdir);
    snprintf(zebra, sizeof(zebra), "%s/zebra.txt", tmpdir);
    snprintf(apple, sizeof(apple), "%s/apple.txt", tmpdir);

    mkdir(zdir, 0755);
    mkdir(adir, 0755);
    touch(zebra);
    touch(apple);

    FileTree *ft = filetree_create(tmpdir);
    ASSERT(ft != NULL, "filetree_create returned non-NULL");

    if (ft) {
        ASSERT(ft->count == 4, "root has 4 entries");

        if (ft->count == 4) {
            /* Directories come first, alphabetically */
            ASSERT(ft->entries[0].is_dir == 1,                   "entry[0] is a dir");
            ASSERT(strcmp(ft->entries[0].name, "adir") == 0,     "entry[0] is 'adir'");
            ASSERT(ft->entries[1].is_dir == 1,                   "entry[1] is a dir");
            ASSERT(strcmp(ft->entries[1].name, "zdir") == 0,     "entry[1] is 'zdir'");

            /* Files come after, alphabetically */
            ASSERT(ft->entries[2].is_dir == 0,                   "entry[2] is a file");
            ASSERT(strcmp(ft->entries[2].name, "apple.txt") == 0,"entry[2] is 'apple.txt'");
            ASSERT(ft->entries[3].is_dir == 0,                   "entry[3] is a file");
            ASSERT(strcmp(ft->entries[3].name, "zebra.txt") == 0,"entry[3] is 'zebra.txt'");
        }

        filetree_free(ft);
    }

    /* Cleanup */
    rmdir(zdir);
    rmdir(adir);
    unlink(zebra);
    unlink(apple);
    rmdir(tmpdir);
}

/* ============================================================================
 * Test: filetree_toggle — expand then collapse
 * ============================================================================ */

TEST(test_toggle_expand_collapse)
{
    char tmpdir[] = "/tmp/texty_XXXXXX";
    char *result  = mkdtemp(tmpdir);
    ASSERT(result != NULL, "mkdtemp created temp dir");
    if (!result) return;

    char src_dir[1024], main_c[1024], utils_c[1024], readme[1024];
    int ok = build_test_tree(tmpdir, src_dir, main_c, utils_c, readme);
    ASSERT(ok == 0, "build_test_tree succeeded");
    if (ok != 0) { rmdir(tmpdir); return; }

    FileTree *ft = filetree_create(tmpdir);
    ASSERT(ft != NULL, "filetree_create returned non-NULL");
    if (!ft) { cleanup_test_tree(tmpdir, src_dir, main_c, utils_c, readme); return; }

    /* Initially collapsed: just src/ and README.md */
    ASSERT(ft->count == 2, "initially 2 entries (src/ collapsed)");
    ASSERT(filetree_is_expanded(ft, src_dir) == 0, "src/ is not expanded initially");

    /* --- EXPAND: toggle index 0 (src/) --- */
    filetree_toggle(ft, 0);

    ASSERT(filetree_is_expanded(ft, src_dir) == 1, "src/ is expanded after toggle");

    /*
     * After expanding src/, the flat list should be:
     *   [0] src/          depth=0
     *   [1] src/main.c    depth=1
     *   [2] src/utils.c   depth=1
     *   [3] README.md     depth=0
     * Total = 4 entries.
     */
    ASSERT(ft->count == 4, "4 entries after expanding src/");

    if (ft->count == 4) {
        ASSERT(strcmp(ft->entries[0].name, "src")      == 0, "entry[0] is 'src'");
        ASSERT(ft->entries[0].is_dir == 1,                   "entry[0] is a dir");
        ASSERT(ft->entries[0].depth  == 0,                   "entry[0] depth is 0");

        ASSERT(strcmp(ft->entries[1].name, "main.c")   == 0, "entry[1] is 'main.c'");
        ASSERT(ft->entries[1].is_dir == 0,                   "entry[1] is a file");
        ASSERT(ft->entries[1].depth  == 1,                   "entry[1] depth is 1");

        ASSERT(strcmp(ft->entries[2].name, "utils.c")  == 0, "entry[2] is 'utils.c'");
        ASSERT(ft->entries[2].is_dir == 0,                   "entry[2] is a file");
        ASSERT(ft->entries[2].depth  == 1,                   "entry[2] depth is 1");

        ASSERT(strcmp(ft->entries[3].name, "README.md") == 0, "entry[3] is 'README.md'");
        ASSERT(ft->entries[3].is_dir == 0,                    "entry[3] is a file");
        ASSERT(ft->entries[3].depth  == 0,                    "entry[3] depth is 0");
    }

    /* --- COLLAPSE: toggle index 0 again (src/) --- */
    filetree_toggle(ft, 0);

    ASSERT(filetree_is_expanded(ft, src_dir) == 0, "src/ is collapsed after second toggle");
    ASSERT(ft->count == 2, "back to 2 entries after collapsing src/");

    filetree_free(ft);
    cleanup_test_tree(tmpdir, src_dir, main_c, utils_c, readme);
}

/* ============================================================================
 * Test: full paths are set correctly
 * ============================================================================ */

TEST(test_full_paths)
{
    char tmpdir[] = "/tmp/texty_XXXXXX";
    char *result  = mkdtemp(tmpdir);
    ASSERT(result != NULL, "mkdtemp created temp dir");
    if (!result) return;

    char src_dir[1024], main_c[1024], utils_c[1024], readme[1024];
    int ok = build_test_tree(tmpdir, src_dir, main_c, utils_c, readme);
    ASSERT(ok == 0, "build_test_tree succeeded");
    if (ok != 0) { cleanup_test_tree(tmpdir, src_dir, main_c, utils_c, readme); return; }

    FileTree *ft = filetree_create(tmpdir);
    ASSERT(ft != NULL, "filetree_create returned non-NULL");
    if (!ft) { cleanup_test_tree(tmpdir, src_dir, main_c, utils_c, readme); return; }

    /* Expand src/ so we can verify nested paths */
    filetree_toggle(ft, 0);

    if (ft->count == 4) {
        /*
         * Verify that path[] contains the full absolute path, not just the
         * basename.  This is important for file-open and toggle operations.
         */
        ASSERT(strcmp(ft->entries[0].path, src_dir)  == 0, "entry[0] full path is src_dir");
        ASSERT(strcmp(ft->entries[1].path, main_c)   == 0, "entry[1] full path is main_c");
        ASSERT(strcmp(ft->entries[2].path, utils_c)  == 0, "entry[2] full path is utils_c");
        ASSERT(strcmp(ft->entries[3].path, readme)   == 0, "entry[3] full path is readme");
    } else {
        ASSERT(0, "expected 4 entries after expand (path test skipped)");
    }

    filetree_free(ft);
    cleanup_test_tree(tmpdir, src_dir, main_c, utils_c, readme);
}

/* ============================================================================
 * Test: filetree_rebuild — changes on disk are picked up
 * ============================================================================ */

TEST(test_rebuild_picks_up_new_file)
{
    char tmpdir[] = "/tmp/texty_XXXXXX";
    char *result  = mkdtemp(tmpdir);
    ASSERT(result != NULL, "mkdtemp created temp dir");
    if (!result) return;

    /* Start with just one file */
    char file_a[1024];
    snprintf(file_a, sizeof(file_a), "%s/a.txt", tmpdir);
    touch(file_a);

    FileTree *ft = filetree_create(tmpdir);
    ASSERT(ft != NULL, "filetree_create returned non-NULL");
    if (!ft) { unlink(file_a); rmdir(tmpdir); return; }

    ASSERT(ft->count == 1, "initially 1 entry");

    /* Create a second file on disk, then rebuild */
    char file_b[1024];
    snprintf(file_b, sizeof(file_b), "%s/b.txt", tmpdir);
    touch(file_b);

    filetree_rebuild(ft);

    ASSERT(ft->count == 2, "2 entries after adding b.txt and rebuilding");

    filetree_free(ft);
    unlink(file_a);
    unlink(file_b);
    rmdir(tmpdir);
}

/* ============================================================================
 * Test: filetree_free is safe to call with NULL
 * ============================================================================ */

TEST(test_free_null_is_safe)
{
    /*
     * free(NULL) is defined to be a no-op in the C standard.
     * filetree_free() calls free(ft), so filetree_free(NULL) should be safe.
     * We just verify it does not crash.
     */
    filetree_free(NULL);
    ASSERT(1, "filetree_free(NULL) did not crash");
}

/* ============================================================================
 * Test: hidden files/dirs are excluded
 * ============================================================================ */

TEST(test_hidden_entries_excluded)
{
    char tmpdir[] = "/tmp/texty_XXXXXX";
    char *result  = mkdtemp(tmpdir);
    ASSERT(result != NULL, "mkdtemp created temp dir");
    if (!result) return;

    /* Create one visible file and one hidden file */
    char visible[1024], hidden_f[1024], hidden_d[1024];
    snprintf(visible,  sizeof(visible),  "%s/visible.txt", tmpdir);
    snprintf(hidden_f, sizeof(hidden_f), "%s/.hidden",     tmpdir);
    snprintf(hidden_d, sizeof(hidden_d), "%s/.git",        tmpdir);

    touch(visible);
    touch(hidden_f);
    mkdir(hidden_d, 0755);

    FileTree *ft = filetree_create(tmpdir);
    ASSERT(ft != NULL, "filetree_create returned non-NULL");

    if (ft) {
        /*
         * Only the visible file should appear.
         * Hidden entries (names starting with '.') are skipped by fill_dir().
         */
        ASSERT(ft->count == 1, "only 1 entry (hidden entries excluded)");
        if (ft->count >= 1) {
            ASSERT(strcmp(ft->entries[0].name, "visible.txt") == 0,
                   "visible entry is visible.txt");
        }
        filetree_free(ft);
    }

    unlink(visible);
    unlink(hidden_f);
    rmdir(hidden_d);
    rmdir(tmpdir);
}

/* ============================================================================
 * Test: collapse removes nested expanded[] entries
 * ============================================================================ */

TEST(test_collapse_clears_nested_expanded)
{
    /*
     * Create a two-level directory tree:
     *   <tmpdir>/
     *     outer/
     *       inner/
     *         deep.txt
     *
     * Expand outer/, then expand inner/, then collapse outer/.
     * After collapsing outer/, inner/ should no longer be in expanded[].
     */
    char tmpdir[] = "/tmp/texty_XXXXXX";
    char *result  = mkdtemp(tmpdir);
    ASSERT(result != NULL, "mkdtemp created temp dir");
    if (!result) return;

    char outer[1024], inner[1024], deep[1024];
    snprintf(outer, sizeof(outer), "%s/outer",           tmpdir);
    snprintf(inner, sizeof(inner), "%s/outer/inner",     tmpdir);
    snprintf(deep,  sizeof(deep),  "%s/outer/inner/deep.txt", tmpdir);

    mkdir(outer, 0755);
    mkdir(inner, 0755);
    touch(deep);

    FileTree *ft = filetree_create(tmpdir);
    ASSERT(ft != NULL, "filetree_create returned non-NULL");
    if (!ft) {
        unlink(deep); rmdir(inner); rmdir(outer); rmdir(tmpdir);
        return;
    }

    /* Initially: just outer/ collapsed */
    ASSERT(ft->count == 1, "initially 1 entry (outer/ collapsed)");

    /* Expand outer/ (index 0) */
    filetree_toggle(ft, 0);
    ASSERT(filetree_is_expanded(ft, outer) == 1, "outer/ is expanded");
    /* Now: outer/, inner/ */
    ASSERT(ft->count == 2, "2 entries: outer/ + inner/");

    /* Expand inner/ (index 1) */
    filetree_toggle(ft, 1);
    ASSERT(filetree_is_expanded(ft, inner) == 1, "inner/ is expanded");
    /* Now: outer/, inner/, deep.txt */
    ASSERT(ft->count == 3, "3 entries: outer/ + inner/ + deep.txt");

    /* Collapse outer/ (index 0) — should also remove inner/ from expanded[] */
    filetree_toggle(ft, 0);

    ASSERT(filetree_is_expanded(ft, outer) == 0, "outer/ is collapsed");
    ASSERT(filetree_is_expanded(ft, inner) == 0, "inner/ also removed from expanded[]");
    ASSERT(ft->count == 1, "back to 1 entry after collapsing outer/");

    filetree_free(ft);
    unlink(deep);
    rmdir(inner);
    rmdir(outer);
    rmdir(tmpdir);
}

/* ============================================================================
 * Test: collapse depth-1 subdirectory while parent stays expanded
 *
 * This is a regression test for a crash caused by strncpy() being called
 * with identical src and dst pointers in the filetree_toggle() collapse loop.
 *
 * Scenario:
 *   1. Expand a root-level directory (parent/).
 *   2. Expand a subdirectory inside it (parent/child/).
 *   3. Collapse child/ while parent/ remains expanded.
 *
 * Before the fix, step 3 crashed because the collapse loop called
 *   strncpy(expanded[0], expanded[0], ...)
 * — a self-copy — when the first entry in expanded[] did not need to be
 * removed.  macOS's optimised strncpy traps on overlapping src/dst.
 * ============================================================================ */

TEST(test_collapse_depth1_child_while_parent_expanded)
{
    /*
     * Directory layout:
     *   <tmpdir>/
     *     parent/
     *       child/       <-- the empty subdirectory we expand then collapse
     *       sibling.txt
     *     root_file.txt
     */
    char tmpdir[] = "/tmp/texty_XXXXXX";
    char *result  = mkdtemp(tmpdir);
    ASSERT(result != NULL, "mkdtemp created temp dir");
    if (!result) return;

    char parent[1024], child[1024], sibling[1024], root_file[1024];
    snprintf(parent,    sizeof(parent),    "%s/parent",           tmpdir);
    snprintf(child,     sizeof(child),     "%s/parent/child",     tmpdir);
    snprintf(sibling,   sizeof(sibling),   "%s/parent/sibling.txt", tmpdir);
    snprintf(root_file, sizeof(root_file), "%s/root_file.txt",    tmpdir);

    mkdir(parent, 0755);
    mkdir(child,  0755);
    touch(sibling);
    touch(root_file);

    FileTree *ft = filetree_create(tmpdir);
    ASSERT(ft != NULL, "filetree_create returned non-NULL");
    if (!ft) { rmdir(child); unlink(sibling); rmdir(parent); unlink(root_file); rmdir(tmpdir); return; }

    /* Step 1: expand parent/ (index 0) */
    ASSERT(ft->count == 2, "initially 2 entries: parent/ + root_file.txt");
    filetree_toggle(ft, 0);
    ASSERT(filetree_is_expanded(ft, parent) == 1, "parent/ is expanded");
    /* parent/, child/, sibling.txt, root_file.txt */
    ASSERT(ft->count == 4, "4 entries after expanding parent/");

    /* Step 2: expand child/ (index 1, depth 1) */
    ASSERT(ft->entries[1].is_dir == 1,                     "entry[1] is a directory");
    ASSERT(strcmp(ft->entries[1].name, "child") == 0,      "entry[1] is child/");
    filetree_toggle(ft, 1);
    ASSERT(filetree_is_expanded(ft, child) == 1, "child/ is expanded");
    /* child/ is empty so count stays the same */
    ASSERT(ft->count == 4, "count unchanged (child/ is empty)");

    /* Step 3: collapse child/ — this previously crashed via strncpy self-overlap */
    filetree_toggle(ft, 1);
    ASSERT(filetree_is_expanded(ft, child)  == 0, "child/ is collapsed");
    ASSERT(filetree_is_expanded(ft, parent) == 1, "parent/ is still expanded");
    ASSERT(ft->count == 4, "count still 4 (child/ was empty)");

    filetree_free(ft);
    rmdir(child);
    unlink(sibling);
    rmdir(parent);
    unlink(root_file);
    rmdir(tmpdir);
}

/* ============================================================================
 * main
 * ============================================================================ */

int main(void)
{
    printf("=== test_filetree ===\n");

    RUN(test_create_basic);
    RUN(test_sort_order);
    RUN(test_toggle_expand_collapse);
    RUN(test_full_paths);
    RUN(test_rebuild_picks_up_new_file);
    RUN(test_free_null_is_safe);
    RUN(test_hidden_entries_excluded);
    RUN(test_collapse_clears_nested_expanded);
    RUN(test_collapse_depth1_child_while_parent_expanded);

    TEST_SUMMARY();
}
