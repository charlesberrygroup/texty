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

    TEST_SUMMARY();
}
