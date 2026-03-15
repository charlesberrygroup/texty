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
 * main
 * ============================================================================ */

int main(void)
{
    printf("=== test_git ===\n");

    RUN(test_git_state_init_free);
    RUN(test_parse_empty_diff);
    RUN(test_parse_simple_add);
    RUN(test_parse_simple_modify);
    RUN(test_parse_simple_delete);
    RUN(test_parse_mixed_hunk);
    RUN(test_parse_multiple_hunks);
    RUN(test_parse_null_diff);

    TEST_SUMMARY();
}
