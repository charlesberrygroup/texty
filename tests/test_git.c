/*
 * test_git.c — Unit tests for git.c
 * =============================================================================
 * Tests the git diff parser without running actual git commands.
 *
 * The key function under test is git_parse_diff_output(), which takes raw
 * unified diff text and populates a GitState's line_status[] array.
 * By feeding known diff strings, we verify that each line gets the correct
 * marker (ADDED, MODIFIED, DELETED, or UNCHANGED).
 * =============================================================================
 */

#include "test_runner.h"
#include "git.h"

#include <stdlib.h>
#include <string.h>

/* ============================================================================
 * Helper — set up a GitState with a pre-allocated line_status array
 * ============================================================================ */

static void setup_state(GitState *gs, int total_lines)
{
    git_state_init(gs);
    gs->line_status = calloc(total_lines, sizeof(GitLineStatus));
    gs->line_count  = total_lines;
    gs->capacity    = total_lines;
}

/* ============================================================================
 * Lifecycle
 * ============================================================================ */

TEST(test_git_state_init_free)
{
    GitState gs;
    git_state_init(&gs);
    ASSERT(gs.line_status == NULL, "line_status is NULL after init");
    ASSERT(gs.repo_root == NULL,   "repo_root is NULL after init");
    ASSERT(gs.is_tracked == 0,     "is_tracked is 0 after init");
    git_state_free(&gs);
    ASSERT(1, "init/free cycle did not crash");
}

/* ============================================================================
 * Diff parsing — simple cases
 * ============================================================================ */

TEST(test_parse_empty_diff)
{
    /* No changes — all lines should remain UNCHANGED */
    GitState gs;
    setup_state(&gs, 5);

    git_parse_diff_output(&gs, "", 5);

    for (int i = 0; i < 5; i++)
        ASSERT(gs.line_status[i] == GIT_LINE_UNCHANGED, "all unchanged");

    git_state_free(&gs);
}

TEST(test_parse_simple_add)
{
    /*
     * Simulated diff: 3 lines added at line 2 (1-based) in a 5-line file.
     *
     * @@ -1,0 +2,3 @@
     * +added line 1
     * +added line 2
     * +added line 3
     *
     * Lines 1 (0-based) through 3 should be ADDED.
     */
    const char *diff =
        "--- a/file.c\n"
        "+++ b/file.c\n"
        "@@ -1,0 +2,3 @@\n"
        "+added line 1\n"
        "+added line 2\n"
        "+added line 3\n";

    GitState gs;
    setup_state(&gs, 5);

    git_parse_diff_output(&gs, diff, 5);

    ASSERT(gs.line_status[0] == GIT_LINE_UNCHANGED, "line 0 unchanged");
    ASSERT(gs.line_status[1] == GIT_LINE_ADDED,     "line 1 added");
    ASSERT(gs.line_status[2] == GIT_LINE_ADDED,     "line 2 added");
    ASSERT(gs.line_status[3] == GIT_LINE_ADDED,     "line 3 added");
    ASSERT(gs.line_status[4] == GIT_LINE_UNCHANGED, "line 4 unchanged");

    git_state_free(&gs);
}

TEST(test_parse_simple_modify)
{
    /*
     * One line changed: old line removed, new line added at the same position.
     *
     * @@ -2,1 +2,1 @@
     * -old content
     * +new content
     *
     * Line 1 (0-based) should be MODIFIED.
     */
    const char *diff =
        "--- a/file.c\n"
        "+++ b/file.c\n"
        "@@ -2,1 +2,1 @@\n"
        "-old content\n"
        "+new content\n";

    GitState gs;
    setup_state(&gs, 5);

    git_parse_diff_output(&gs, diff, 5);

    ASSERT(gs.line_status[0] == GIT_LINE_UNCHANGED, "line 0 unchanged");
    ASSERT(gs.line_status[1] == GIT_LINE_MODIFIED,  "line 1 modified");
    ASSERT(gs.line_status[2] == GIT_LINE_UNCHANGED, "line 2 unchanged");

    git_state_free(&gs);
}

TEST(test_parse_simple_delete)
{
    /*
     * Two lines deleted after line 3.
     *
     * @@ -3,3 +3,1 @@
     *  context line 3
     * -deleted line 1
     * -deleted line 2
     *
     * Line 2 (0-based, the context line) should get DELETED marker.
     */
    const char *diff =
        "--- a/file.c\n"
        "+++ b/file.c\n"
        "@@ -3,3 +3,1 @@\n"
        " context line 3\n"
        "-deleted line 1\n"
        "-deleted line 2\n";

    GitState gs;
    setup_state(&gs, 5);

    git_parse_diff_output(&gs, diff, 5);

    ASSERT(gs.line_status[1] == GIT_LINE_UNCHANGED, "line 1 unchanged");
    ASSERT(gs.line_status[2] == GIT_LINE_DELETED,   "line 2 has DELETED marker");
    ASSERT(gs.line_status[3] == GIT_LINE_UNCHANGED, "line 3 unchanged");

    git_state_free(&gs);
}

TEST(test_parse_mixed_hunk)
{
    /*
     * Mixed changes: 1 modified line + 1 added line.
     *
     * @@ -1,2 +1,3 @@
     *  unchanged first line
     * -old second line
     * +new second line
     * +brand new third line
     *
     * Line 1 (0-based) = MODIFIED, line 2 = ADDED.
     */
    const char *diff =
        "--- a/file.c\n"
        "+++ b/file.c\n"
        "@@ -1,2 +1,3 @@\n"
        " unchanged first line\n"
        "-old second line\n"
        "+new second line\n"
        "+brand new third line\n";

    GitState gs;
    setup_state(&gs, 5);

    git_parse_diff_output(&gs, diff, 5);

    ASSERT(gs.line_status[0] == GIT_LINE_UNCHANGED, "line 0 context");
    ASSERT(gs.line_status[1] == GIT_LINE_MODIFIED,  "line 1 modified");
    ASSERT(gs.line_status[2] == GIT_LINE_ADDED,     "line 2 added");

    git_state_free(&gs);
}

TEST(test_parse_multiple_hunks)
{
    /*
     * Two separate hunks in one diff.
     *
     * @@ -1,1 +1,1 @@
     * -old line 1
     * +new line 1
     * @@ -5,1 +5,1 @@
     * -old line 5
     * +new line 5
     *
     * Lines 0 and 4 (0-based) should be MODIFIED.
     */
    const char *diff =
        "--- a/file.c\n"
        "+++ b/file.c\n"
        "@@ -1,1 +1,1 @@\n"
        "-old line 1\n"
        "+new line 1\n"
        "@@ -5,1 +5,1 @@\n"
        "-old line 5\n"
        "+new line 5\n";

    GitState gs;
    setup_state(&gs, 10);

    git_parse_diff_output(&gs, diff, 10);

    ASSERT(gs.line_status[0] == GIT_LINE_MODIFIED,  "line 0 modified (hunk 1)");
    ASSERT(gs.line_status[1] == GIT_LINE_UNCHANGED, "line 1 unchanged");
    ASSERT(gs.line_status[4] == GIT_LINE_MODIFIED,  "line 4 modified (hunk 2)");

    git_state_free(&gs);
}

TEST(test_parse_null_diff)
{
    /* NULL diff text should not crash */
    GitState gs;
    setup_state(&gs, 3);

    int result = git_parse_diff_output(&gs, NULL, 3);
    ASSERT(result == 0, "NULL diff returns 0");

    git_state_free(&gs);
}

/* ============================================================================
 * Chunk extraction — for inline diff view
 *
 * These tests verify git_extract_chunks(), which parses unified diff text
 * into positioned chunks of removed lines.  Each chunk has a `before_line`
 * (where phantom lines should appear) and an array of old line strings.
 * ============================================================================ */

TEST(test_chunks_empty_diff)
{
    /* No diff text → no chunks */
    GitDiffChunks dc;
    memset(&dc, 0, sizeof(dc));

    int result = git_extract_chunks(&dc, "");
    ASSERT(result == 0,    "empty diff returns 0");
    ASSERT(dc.count == 0,  "no chunks from empty diff");

    git_diff_chunks_free(&dc);
}

TEST(test_chunks_null_diff)
{
    /* NULL diff text should not crash */
    GitDiffChunks dc;
    memset(&dc, 0, sizeof(dc));

    int result = git_extract_chunks(&dc, NULL);
    ASSERT(result == 0,    "NULL diff returns 0");
    ASSERT(dc.count == 0,  "no chunks from NULL diff");

    git_diff_chunks_free(&dc);
}

TEST(test_chunks_simple_modify)
{
    /*
     * One line changed at position 2 (1-based).
     *
     * @@ -2,1 +2,1 @@
     * -old content
     * +new content
     *
     * Should produce one chunk: before_line=1 (0-based), old=["old content"].
     */
    const char *diff =
        "--- a/file.c\n"
        "+++ b/file.c\n"
        "@@ -2,1 +2,1 @@\n"
        "-old content\n"
        "+new content\n";

    GitDiffChunks dc;
    memset(&dc, 0, sizeof(dc));

    int result = git_extract_chunks(&dc, diff);
    ASSERT(result == 0,     "extract returns 0");
    ASSERT(dc.count == 1,   "one chunk");

    ASSERT(dc.chunks[0].before_line == 1,  "before_line is 1 (0-based)");
    ASSERT(dc.chunks[0].old_count == 1,    "one old line");
    ASSERT(strcmp(dc.chunks[0].old_lines[0], "old content") == 0,
           "old line content matches");

    git_diff_chunks_free(&dc);
}

TEST(test_chunks_simple_delete)
{
    /*
     * Two lines deleted after line 3.
     *
     * @@ -3,3 +3,1 @@
     *  context line 3
     * -deleted line 1
     * -deleted line 2
     *
     * The context line at new_line=2 is processed first, advancing to 3.
     * Then the '-' lines start at new_line=3.
     * Chunk: before_line=3 (0-based), old=["deleted line 1", "deleted line 2"].
     */
    const char *diff =
        "--- a/file.c\n"
        "+++ b/file.c\n"
        "@@ -3,3 +3,1 @@\n"
        " context line 3\n"
        "-deleted line 1\n"
        "-deleted line 2\n";

    GitDiffChunks dc;
    memset(&dc, 0, sizeof(dc));

    git_extract_chunks(&dc, diff);
    ASSERT(dc.count == 1,  "one chunk");
    ASSERT(dc.chunks[0].before_line == 3,  "before_line is 3 (after context)");
    ASSERT(dc.chunks[0].old_count == 2,    "two old lines");
    ASSERT(strcmp(dc.chunks[0].old_lines[0], "deleted line 1") == 0,
           "first old line");
    ASSERT(strcmp(dc.chunks[0].old_lines[1], "deleted line 2") == 0,
           "second old line");

    git_diff_chunks_free(&dc);
}

TEST(test_chunks_pure_addition)
{
    /*
     * Lines only added — no removals.
     *
     * @@ -1,0 +2,2 @@
     * +added line 1
     * +added line 2
     *
     * No old lines to show → no chunks.
     */
    const char *diff =
        "--- a/file.c\n"
        "+++ b/file.c\n"
        "@@ -1,0 +2,2 @@\n"
        "+added line 1\n"
        "+added line 2\n";

    GitDiffChunks dc;
    memset(&dc, 0, sizeof(dc));

    git_extract_chunks(&dc, diff);
    ASSERT(dc.count == 0,  "no chunks for pure addition");

    git_diff_chunks_free(&dc);
}

TEST(test_chunks_multiple_groups_in_hunk)
{
    /*
     * A single hunk with two separate change groups separated by context.
     *
     * @@ -1,6 +1,6 @@
     *  unchanged first line
     * -old second line
     * +new second line
     *  unchanged third line
     * -old fourth line
     * +new fourth line
     *  unchanged fifth line
     *
     * Should produce two chunks:
     *   chunk 0: before_line=1, old=["old second line"]
     *   chunk 1: before_line=3, old=["old fourth line"]
     */
    const char *diff =
        "--- a/file.c\n"
        "+++ b/file.c\n"
        "@@ -1,6 +1,6 @@\n"
        " unchanged first line\n"
        "-old second line\n"
        "+new second line\n"
        " unchanged third line\n"
        "-old fourth line\n"
        "+new fourth line\n"
        " unchanged fifth line\n";

    GitDiffChunks dc;
    memset(&dc, 0, sizeof(dc));

    git_extract_chunks(&dc, diff);
    ASSERT(dc.count == 2,  "two chunks from separated groups");

    ASSERT(dc.chunks[0].before_line == 1,  "chunk 0 before_line");
    ASSERT(dc.chunks[0].old_count == 1,    "chunk 0 old_count");
    ASSERT(strcmp(dc.chunks[0].old_lines[0], "old second line") == 0,
           "chunk 0 content");

    ASSERT(dc.chunks[1].before_line == 3,  "chunk 1 before_line");
    ASSERT(dc.chunks[1].old_count == 1,    "chunk 1 old_count");
    ASSERT(strcmp(dc.chunks[1].old_lines[0], "old fourth line") == 0,
           "chunk 1 content");

    git_diff_chunks_free(&dc);
}

TEST(test_chunks_multiple_hunks)
{
    /*
     * Two separate hunks in the diff.
     *
     * @@ -1,1 +1,1 @@
     * -old line 1
     * +new line 1
     * @@ -10,1 +10,1 @@
     * -old line 10
     * +new line 10
     *
     * Two chunks: before_line=0 and before_line=9.
     */
    const char *diff =
        "--- a/file.c\n"
        "+++ b/file.c\n"
        "@@ -1,1 +1,1 @@\n"
        "-old line 1\n"
        "+new line 1\n"
        "@@ -10,1 +10,1 @@\n"
        "-old line 10\n"
        "+new line 10\n";

    GitDiffChunks dc;
    memset(&dc, 0, sizeof(dc));

    git_extract_chunks(&dc, diff);
    ASSERT(dc.count == 2,  "two chunks from two hunks");

    ASSERT(dc.chunks[0].before_line == 0,  "hunk 1 before_line");
    ASSERT(dc.chunks[0].old_count == 1,    "hunk 1 old_count");
    ASSERT(strcmp(dc.chunks[0].old_lines[0], "old line 1") == 0,
           "hunk 1 content");

    ASSERT(dc.chunks[1].before_line == 9,  "hunk 2 before_line");
    ASSERT(dc.chunks[1].old_count == 1,    "hunk 2 old_count");
    ASSERT(strcmp(dc.chunks[1].old_lines[0], "old line 10") == 0,
           "hunk 2 content");

    git_diff_chunks_free(&dc);
}

TEST(test_chunks_deletion_at_start)
{
    /*
     * Lines deleted at the very beginning of the file.
     *
     * @@ -1,2 +1,0 @@
     * -first line
     * -second line
     *
     * Chunk: before_line=0 (before the first line of the new file).
     */
    const char *diff =
        "--- a/file.c\n"
        "+++ b/file.c\n"
        "@@ -1,2 +1,0 @@\n"
        "-first line\n"
        "-second line\n";

    GitDiffChunks dc;
    memset(&dc, 0, sizeof(dc));

    git_extract_chunks(&dc, diff);
    ASSERT(dc.count == 1,              "one chunk");
    ASSERT(dc.chunks[0].before_line == 0,  "before_line is 0 (start of file)");
    ASSERT(dc.chunks[0].old_count == 2,    "two old lines");

    git_diff_chunks_free(&dc);
}

TEST(test_chunks_multi_line_modify)
{
    /*
     * Three lines replaced by two lines.
     *
     * @@ -2,3 +2,2 @@
     * -old line A
     * -old line B
     * -old line C
     * +new line A
     * +new line B
     *
     * Chunk: before_line=1, 3 old lines.
     */
    const char *diff =
        "--- a/file.c\n"
        "+++ b/file.c\n"
        "@@ -2,3 +2,2 @@\n"
        "-old line A\n"
        "-old line B\n"
        "-old line C\n"
        "+new line A\n"
        "+new line B\n";

    GitDiffChunks dc;
    memset(&dc, 0, sizeof(dc));

    git_extract_chunks(&dc, diff);
    ASSERT(dc.count == 1,              "one chunk");
    ASSERT(dc.chunks[0].before_line == 1,  "before_line is 1");
    ASSERT(dc.chunks[0].old_count == 3,    "three old lines");
    ASSERT(strcmp(dc.chunks[0].old_lines[0], "old line A") == 0, "old A");
    ASSERT(strcmp(dc.chunks[0].old_lines[1], "old line B") == 0, "old B");
    ASSERT(strcmp(dc.chunks[0].old_lines[2], "old line C") == 0, "old C");

    git_diff_chunks_free(&dc);
}

/* ============================================================================
 * Phantom line counting — for cursor offset calculation
 * ============================================================================ */

TEST(test_phantom_lines_in_range)
{
    /*
     * Set up chunks manually and verify the counting function.
     *
     * chunks[0]: before_line=2, old_count=3
     * chunks[1]: before_line=5, old_count=1
     * chunks[2]: before_line=8, old_count=2
     */
    GitDiffChunks dc;
    memset(&dc, 0, sizeof(dc));

    dc.chunks = malloc(3 * sizeof(GitDiffChunk));
    dc.count = 3;
    dc.capacity = 3;

    dc.chunks[0].before_line = 2;
    dc.chunks[0].old_count   = 3;
    dc.chunks[0].old_lines   = NULL;  /* not needed for counting */

    dc.chunks[1].before_line = 5;
    dc.chunks[1].old_count   = 1;
    dc.chunks[1].old_lines   = NULL;

    dc.chunks[2].before_line = 8;
    dc.chunks[2].old_count   = 2;
    dc.chunks[2].old_lines   = NULL;

    /* Range [0, 10) — all chunks */
    ASSERT(git_phantom_lines_in_range(&dc, 0, 10) == 6,
           "all 6 phantom lines in [0,10)");

    /* Range [0, 3) — only chunk at before_line=2 */
    ASSERT(git_phantom_lines_in_range(&dc, 0, 3) == 3,
           "3 phantom lines in [0,3)");

    /* Range [3, 6) — only chunk at before_line=5 */
    ASSERT(git_phantom_lines_in_range(&dc, 3, 6) == 1,
           "1 phantom line in [3,6)");

    /* Range [0, 2) — no chunks */
    ASSERT(git_phantom_lines_in_range(&dc, 0, 2) == 0,
           "0 phantom lines in [0,2)");

    /* Range [6, 8) — no chunks */
    ASSERT(git_phantom_lines_in_range(&dc, 6, 8) == 0,
           "0 phantom lines in [6,8)");

    /* Empty range */
    ASSERT(git_phantom_lines_in_range(&dc, 5, 5) == 0,
           "0 phantom lines in empty range");

    /* NULL chunks */
    ASSERT(git_phantom_lines_in_range(NULL, 0, 10) == 0,
           "0 phantom lines from NULL");

    free(dc.chunks);
}

TEST(test_chunks_free_idempotent)
{
    /* Freeing a zeroed struct should not crash */
    GitDiffChunks dc;
    memset(&dc, 0, sizeof(dc));

    git_diff_chunks_free(&dc);
    ASSERT(dc.count == 0,    "count is 0 after free");
    ASSERT(dc.chunks == NULL, "chunks is NULL after free");

    /* Double free should be safe */
    git_diff_chunks_free(&dc);
    ASSERT(1, "double free did not crash");
}

/* ============================================================================
 * Hunk patch extraction — for staging individual hunks
 *
 * These tests verify git_build_hunk_patch(), which extracts a single hunk
 * from a full unified diff as a valid patch that can be piped to
 * `git apply --cached`.
 * ============================================================================ */

TEST(test_patch_null_diff)
{
    char *patch = git_build_hunk_patch(NULL, 0);
    ASSERT(patch == NULL, "NULL diff returns NULL");
}

TEST(test_patch_empty_diff)
{
    char *patch = git_build_hunk_patch("", 0);
    ASSERT(patch == NULL, "empty diff returns NULL");
}

TEST(test_patch_no_matching_hunk)
{
    /*
     * Hunk covers lines 0..0 (1-based line 1, count 1 → 0-based line 0).
     * Searching for line 5 should return NULL.
     */
    const char *diff =
        "diff --git a/f.c b/f.c\n"
        "index abc..def 100644\n"
        "--- a/f.c\n"
        "+++ b/f.c\n"
        "@@ -1,1 +1,1 @@\n"
        "-old\n"
        "+new\n";

    char *patch = git_build_hunk_patch(diff, 5);
    ASSERT(patch == NULL, "no hunk at line 5");
}

TEST(test_patch_single_hunk)
{
    /*
     * Single hunk: modification at line 1 (1-based) → 0-based line 0.
     * The extracted patch should contain the file header + that hunk.
     */
    const char *diff =
        "diff --git a/f.c b/f.c\n"
        "index abc..def 100644\n"
        "--- a/f.c\n"
        "+++ b/f.c\n"
        "@@ -1,1 +1,1 @@\n"
        "-old line\n"
        "+new line\n";

    char *patch = git_build_hunk_patch(diff, 0);
    ASSERT(patch != NULL, "patch extracted for line 0");

    /* The patch should start with the file header */
    ASSERT(strstr(patch, "diff --git a/f.c b/f.c") != NULL,
           "patch contains diff header");
    ASSERT(strstr(patch, "--- a/f.c") != NULL,
           "patch contains --- line");
    ASSERT(strstr(patch, "+++ b/f.c") != NULL,
           "patch contains +++ line");

    /* And contain the hunk */
    ASSERT(strstr(patch, "@@ -1,1 +1,1 @@") != NULL,
           "patch contains hunk header");
    ASSERT(strstr(patch, "-old line") != NULL,
           "patch contains removed line");
    ASSERT(strstr(patch, "+new line") != NULL,
           "patch contains added line");

    free(patch);
}

TEST(test_patch_selects_correct_hunk)
{
    /*
     * Two hunks: line 1 and line 10 (1-based).
     * Asking for line 9 (0-based) should get the second hunk.
     */
    const char *diff =
        "diff --git a/f.c b/f.c\n"
        "index abc..def 100644\n"
        "--- a/f.c\n"
        "+++ b/f.c\n"
        "@@ -1,1 +1,1 @@\n"
        "-first old\n"
        "+first new\n"
        "@@ -10,1 +10,1 @@\n"
        "-second old\n"
        "+second new\n";

    /* Line 0 should get hunk 1 */
    char *patch1 = git_build_hunk_patch(diff, 0);
    ASSERT(patch1 != NULL, "patch for line 0");
    ASSERT(strstr(patch1, "-first old") != NULL,  "hunk 1 old line");
    ASSERT(strstr(patch1, "-second old") == NULL, "hunk 2 NOT in patch 1");
    free(patch1);

    /* Line 9 should get hunk 2 */
    char *patch2 = git_build_hunk_patch(diff, 9);
    ASSERT(patch2 != NULL, "patch for line 9");
    ASSERT(strstr(patch2, "-second old") != NULL, "hunk 2 old line");
    ASSERT(strstr(patch2, "-first old") == NULL,  "hunk 1 NOT in patch 2");
    free(patch2);
}

TEST(test_patch_multi_line_hunk)
{
    /*
     * Hunk covers lines 5..7 (1-based 5, count 3).
     * Any of lines 4, 5, 6 (0-based) should match.
     */
    const char *diff =
        "diff --git a/f.c b/f.c\n"
        "index abc..def 100644\n"
        "--- a/f.c\n"
        "+++ b/f.c\n"
        "@@ -5,3 +5,3 @@\n"
        "-line a\n"
        "-line b\n"
        "-line c\n"
        "+LINE A\n"
        "+LINE B\n"
        "+LINE C\n";

    /* All three lines should match this hunk */
    char *p4 = git_build_hunk_patch(diff, 4);
    ASSERT(p4 != NULL, "line 4 matches");
    ASSERT(strstr(p4, "-line a") != NULL, "correct hunk for line 4");
    free(p4);

    char *p5 = git_build_hunk_patch(diff, 5);
    ASSERT(p5 != NULL, "line 5 matches");
    free(p5);

    char *p6 = git_build_hunk_patch(diff, 6);
    ASSERT(p6 != NULL, "line 6 matches");
    free(p6);

    /* Line 3 and 7 should NOT match */
    char *p3 = git_build_hunk_patch(diff, 3);
    ASSERT(p3 == NULL, "line 3 does not match");

    char *p7 = git_build_hunk_patch(diff, 7);
    ASSERT(p7 == NULL, "line 7 does not match");
}

TEST(test_patch_deletion_hunk)
{
    /*
     * Pure deletion: 2 lines deleted, new_count=0.
     * @@ -3,2 +3,0 @@ means lines were deleted at position 3 (1-based)
     * in the new file.  The DELETED marker goes on line 2 (0-based).
     *
     * The hunk should match line 2 (the line above the deletion point).
     */
    const char *diff =
        "diff --git a/f.c b/f.c\n"
        "index abc..def 100644\n"
        "--- a/f.c\n"
        "+++ b/f.c\n"
        "@@ -3,2 +3,0 @@\n"
        "-removed a\n"
        "-removed b\n";

    /* Line 2 (0-based) should match — it's the line before the deletion */
    char *patch = git_build_hunk_patch(diff, 2);
    ASSERT(patch != NULL, "deletion hunk matches line 2");
    ASSERT(strstr(patch, "-removed a") != NULL, "contains removed lines");
    free(patch);

    /* Line 1 should not match */
    char *p1 = git_build_hunk_patch(diff, 1);
    ASSERT(p1 == NULL, "line 1 does not match deletion hunk");
}

TEST(test_patch_hunk_with_context)
{
    /*
     * Hunk with context lines.
     * @@ -5,5 +5,5 @@ means 5 lines starting at line 5 (1-based),
     * so 0-based lines 4..8.
     * Even context lines within the hunk should match.
     */
    const char *diff =
        "diff --git a/f.c b/f.c\n"
        "index abc..def 100644\n"
        "--- a/f.c\n"
        "+++ b/f.c\n"
        "@@ -5,5 +5,5 @@\n"
        " context 1\n"
        " context 2\n"
        "-old mid\n"
        "+new mid\n"
        " context 3\n";

    /* Context line at 0-based 4 should match (it's in the hunk range) */
    char *p4 = git_build_hunk_patch(diff, 4);
    ASSERT(p4 != NULL, "context line 4 matches");
    free(p4);

    /* Changed line at 0-based 6 should match */
    char *p6 = git_build_hunk_patch(diff, 6);
    ASSERT(p6 != NULL, "changed line 6 matches");
    free(p6);

    /* Line 3 (before hunk) should not match */
    char *p3 = git_build_hunk_patch(diff, 3);
    ASSERT(p3 == NULL, "line 3 outside hunk");
}

TEST(test_patch_ends_with_newline)
{
    /*
     * Simulate what run_command() produces: the diff text has its trailing
     * newline stripped.  The extracted patch must still end with '\n' or
     * git apply will reject it as "corrupt patch".
     */
    const char *diff =
        "diff --git a/f.c b/f.c\n"
        "index abc..def 100644\n"
        "--- a/f.c\n"
        "+++ b/f.c\n"
        "@@ -1,1 +1,1 @@\n"
        "-old line\n"
        "+new line";   /* <-- no trailing newline, like run_command output */

    char *patch = git_build_hunk_patch(diff, 0);
    ASSERT(patch != NULL, "patch extracted");

    int len = (int)strlen(patch);
    ASSERT(len > 0 && patch[len - 1] == '\n',
           "patch ends with newline");

    free(patch);
}

/* ============================================================================
 * Commit helpers — edge cases (no real repo needed)
 * ============================================================================ */

TEST(test_commit_null_args)
{
    /* NULL/empty args should return -1 without crashing */
    ASSERT(git_has_staged_changes(NULL) == -1,  "NULL repo_root returns -1");
    ASSERT(git_has_staged_changes("")   == -1,  "empty repo_root returns -1");

    ASSERT(git_commit(NULL, "msg")  == -1, "NULL repo_root returns -1");
    ASSERT(git_commit("/tmp", NULL) == -1, "NULL message returns -1");
    ASSERT(git_commit("/tmp", "")   == -1, "empty message returns -1");
}

/* ============================================================================
 * Blame parsing
 *
 * These tests verify git_parse_blame_output(), which parses the output of
 * `git blame --porcelain` into GitBlameLine structs (author, date, sha).
 * ============================================================================ */

TEST(test_blame_parse_null)
{
    GitBlameData bd;
    memset(&bd, 0, sizeof(bd));

    ASSERT(git_parse_blame_output(&bd, NULL, 5) == 0, "NULL text returns 0");
    ASSERT(bd.count == 0, "no lines from NULL text");

    git_blame_free(&bd);
}

TEST(test_blame_parse_empty)
{
    GitBlameData bd;
    memset(&bd, 0, sizeof(bd));

    ASSERT(git_parse_blame_output(&bd, "", 5) == 0, "empty text returns 0");
    ASSERT(bd.count == 0, "no lines from empty text");

    git_blame_free(&bd);
}

TEST(test_blame_parse_single_line)
{
    /*
     * Porcelain output for one commit, one line.
     * The SHA line has 3 numbers: orig_line final_line group_count.
     * Then author, author-time headers, then a tab-prefixed content line.
     */
    const char *text =
        "abcdef1234567890abcdef1234567890abcdef12 1 1 1\n"
        "author John Doe\n"
        "author-mail <john@example.com>\n"
        "author-time 1617283200\n"
        "author-tz -0700\n"
        "committer John Doe\n"
        "committer-mail <john@example.com>\n"
        "committer-time 1617283200\n"
        "committer-tz -0700\n"
        "summary Some commit message\n"
        "filename file.c\n"
        "\tint main() {\n";

    GitBlameData bd;
    memset(&bd, 0, sizeof(bd));

    int result = git_parse_blame_output(&bd, text, 1);
    ASSERT(result == 0, "parse succeeds");
    ASSERT(bd.count == 1, "one line");
    ASSERT(strcmp(bd.lines[0].author, "John Doe") == 0, "author");
    ASSERT(strcmp(bd.lines[0].date, "2021-04-01") == 0, "date from epoch");
    ASSERT(strcmp(bd.lines[0].sha_short, "abcdef1") == 0, "sha_short");

    git_blame_free(&bd);
}

TEST(test_blame_parse_multi_line_same_commit)
{
    /*
     * Two lines from the same commit.  The first occurrence has 3 numbers
     * and metadata headers.  The second has only 2 numbers (no metadata).
     */
    const char *text =
        "abcdef1234567890abcdef1234567890abcdef12 1 1 2\n"
        "author Alice\n"
        "author-mail <alice@test.com>\n"
        "author-time 1700000000\n"
        "author-tz +0000\n"
        "committer Alice\n"
        "committer-mail <alice@test.com>\n"
        "committer-time 1700000000\n"
        "committer-tz +0000\n"
        "summary First commit\n"
        "filename test.c\n"
        "\tline one\n"
        "abcdef1234567890abcdef1234567890abcdef12 2 2\n"
        "\tline two\n";

    GitBlameData bd;
    memset(&bd, 0, sizeof(bd));

    git_parse_blame_output(&bd, text, 2);
    ASSERT(bd.count == 2, "two lines");
    ASSERT(strcmp(bd.lines[0].author, "Alice") == 0, "line 0 author");
    ASSERT(strcmp(bd.lines[1].author, "Alice") == 0, "line 1 author (cached)");
    ASSERT(strcmp(bd.lines[0].date, bd.lines[1].date) == 0,
           "same date for both lines");

    git_blame_free(&bd);
}

TEST(test_blame_parse_multiple_commits)
{
    /*
     * Two lines from different commits.
     */
    const char *text =
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa 1 1 1\n"
        "author Alice\n"
        "author-mail <a@t.com>\n"
        "author-time 1700000000\n"
        "author-tz +0000\n"
        "committer Alice\n"
        "committer-mail <a@t.com>\n"
        "committer-time 1700000000\n"
        "committer-tz +0000\n"
        "summary Commit A\n"
        "filename f.c\n"
        "\tline one\n"
        "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb 1 2 1\n"
        "author Bob\n"
        "author-mail <b@t.com>\n"
        "author-time 1710000000\n"
        "author-tz +0000\n"
        "committer Bob\n"
        "committer-mail <b@t.com>\n"
        "committer-time 1710000000\n"
        "committer-tz +0000\n"
        "summary Commit B\n"
        "filename f.c\n"
        "\tline two\n";

    GitBlameData bd;
    memset(&bd, 0, sizeof(bd));

    git_parse_blame_output(&bd, text, 2);
    ASSERT(bd.count == 2, "two lines");
    ASSERT(strcmp(bd.lines[0].author, "Alice") == 0, "line 0 = Alice");
    ASSERT(strcmp(bd.lines[1].author, "Bob") == 0,   "line 1 = Bob");
    ASSERT(strcmp(bd.lines[0].sha_short, "aaaaaaa") == 0, "sha 0");
    ASSERT(strcmp(bd.lines[1].sha_short, "bbbbbbb") == 0, "sha 1");

    git_blame_free(&bd);
}

TEST(test_blame_parse_uncommitted)
{
    /*
     * Uncommitted changes show all-zeros SHA.
     * Should get "Not committed" as author and empty date.
     */
    const char *text =
        "0000000000000000000000000000000000000000 1 1 1\n"
        "author Not Committed Yet\n"
        "author-mail <not.committed.yet>\n"
        "author-time 1700000000\n"
        "author-tz +0000\n"
        "committer Not Committed Yet\n"
        "committer-mail <not.committed.yet>\n"
        "committer-time 1700000000\n"
        "committer-tz +0000\n"
        "summary \n"
        "filename f.c\n"
        "\tnew line\n";

    GitBlameData bd;
    memset(&bd, 0, sizeof(bd));

    git_parse_blame_output(&bd, text, 1);
    ASSERT(bd.count == 1, "one line");
    ASSERT(strcmp(bd.lines[0].author, "Not committed") == 0,
           "uncommitted author");
    ASSERT(bd.lines[0].date[0] == '\0', "uncommitted has no date");

    git_blame_free(&bd);
}

TEST(test_blame_parse_author_truncation)
{
    /*
     * Author name longer than 31 chars should be safely truncated.
     */
    const char *text =
        "abcdef1234567890abcdef1234567890abcdef12 1 1 1\n"
        "author This Is A Very Long Author Name That Exceeds The Buffer\n"
        "author-mail <long@example.com>\n"
        "author-time 1617283200\n"
        "author-tz +0000\n"
        "committer Test\n"
        "committer-mail <test@t.com>\n"
        "committer-time 1617283200\n"
        "committer-tz +0000\n"
        "summary Truncation test\n"
        "filename f.c\n"
        "\tcontent\n";

    GitBlameData bd;
    memset(&bd, 0, sizeof(bd));

    git_parse_blame_output(&bd, text, 1);
    ASSERT(bd.count == 1, "one line");
    ASSERT((int)strlen(bd.lines[0].author) <= 31,
           "author truncated to fit buffer");

    git_blame_free(&bd);
}

TEST(test_blame_free_idempotent)
{
    GitBlameData bd;
    memset(&bd, 0, sizeof(bd));

    git_blame_free(&bd);
    ASSERT(bd.lines == NULL, "lines NULL after free");

    git_blame_free(&bd);
    ASSERT(1, "double free did not crash");
}

/* ============================================================================
 * main
 * ============================================================================ */

int main(void)
{
    printf("=== test_git ===\n");

    /* Diff parsing (line markers) */
    RUN(test_git_state_init_free);
    RUN(test_parse_empty_diff);
    RUN(test_parse_simple_add);
    RUN(test_parse_simple_modify);
    RUN(test_parse_simple_delete);
    RUN(test_parse_mixed_hunk);
    RUN(test_parse_multiple_hunks);
    RUN(test_parse_null_diff);

    /* Chunk extraction (inline diff) */
    RUN(test_chunks_empty_diff);
    RUN(test_chunks_null_diff);
    RUN(test_chunks_simple_modify);
    RUN(test_chunks_simple_delete);
    RUN(test_chunks_pure_addition);
    RUN(test_chunks_multiple_groups_in_hunk);
    RUN(test_chunks_multiple_hunks);
    RUN(test_chunks_deletion_at_start);
    RUN(test_chunks_multi_line_modify);
    RUN(test_phantom_lines_in_range);
    RUN(test_chunks_free_idempotent);

    /* Hunk patch extraction (staging) */
    RUN(test_patch_null_diff);
    RUN(test_patch_empty_diff);
    RUN(test_patch_no_matching_hunk);
    RUN(test_patch_single_hunk);
    RUN(test_patch_selects_correct_hunk);
    RUN(test_patch_multi_line_hunk);
    RUN(test_patch_deletion_hunk);
    RUN(test_patch_hunk_with_context);
    RUN(test_patch_ends_with_newline);

    /* Commit edge cases */
    RUN(test_commit_null_args);

    /* Blame parsing */
    RUN(test_blame_parse_null);
    RUN(test_blame_parse_empty);
    RUN(test_blame_parse_single_line);
    RUN(test_blame_parse_multi_line_same_commit);
    RUN(test_blame_parse_multiple_commits);
    RUN(test_blame_parse_uncommitted);
    RUN(test_blame_parse_author_truncation);
    RUN(test_blame_free_idempotent);

    TEST_SUMMARY();
}
