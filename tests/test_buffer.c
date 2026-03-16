/*
 * test_buffer.c — Unit tests for buffer.c
 * =============================================================================
 * Tests the core text-buffer operations: create/destroy, insert/delete chars,
 * newline splitting, line joining, region deletion, and queries.
 *
 * Compile and run via: make test
 * =============================================================================
 */

#include "test_runner.h"
#include "buffer.h"

#include <string.h>   /* strcmp, strlen */

/* ============================================================================
 * Lifecycle
 * ============================================================================ */

TEST(test_create_has_one_empty_line)
{
    /*
     * A freshly created buffer should have exactly one line and that line
     * should be empty (length 0, content "").
     */
    Buffer *buf = buffer_create();
    ASSERT(buf != NULL,                    "buffer_create returned NULL");
    ASSERT(buf->num_lines == 1,            "new buffer has 1 line");
    ASSERT(buffer_line_len(buf, 0) == 0,   "line 0 is empty");
    ASSERT(strcmp(buffer_get_line(buf, 0), "") == 0, "line 0 text is empty string");
    ASSERT(buf->dirty == 0,                "new buffer is not dirty");
    buffer_destroy(buf);
}

/* ============================================================================
 * buffer_insert_char
 * ============================================================================ */

TEST(test_insert_char_basic)
{
    /* Insert a single character at column 0 of an empty line. */
    Buffer *buf = buffer_create();
    buffer_insert_char(buf, 0, 0, 'A');
    ASSERT(buffer_line_len(buf, 0) == 1,          "length is 1 after insert");
    ASSERT(buffer_get_line(buf, 0)[0] == 'A',     "char at col 0 is 'A'");
    ASSERT(buf->dirty,                            "buffer is dirty after insert");
    buffer_destroy(buf);
}

TEST(test_insert_char_builds_string)
{
    /* Inserting characters one at a time should build "hello". */
    Buffer *buf = buffer_create();
    const char *word = "hello";
    for (int i = 0; word[i]; i++)
        buffer_insert_char(buf, 0, i, word[i]);

    ASSERT(buffer_line_len(buf, 0) == 5,               "length is 5");
    ASSERT(strcmp(buffer_get_line(buf, 0), "hello") == 0, "content is 'hello'");
    buffer_destroy(buf);
}

TEST(test_insert_char_middle)
{
    /* Insert in the middle of an existing string: "hllo" → "hello". */
    Buffer *buf = buffer_create();
    buffer_insert_char(buf, 0, 0, 'h');
    buffer_insert_char(buf, 0, 1, 'l');
    buffer_insert_char(buf, 0, 2, 'l');
    buffer_insert_char(buf, 0, 3, 'o');
    /* Now insert 'e' at position 1 */
    buffer_insert_char(buf, 0, 1, 'e');

    ASSERT(strcmp(buffer_get_line(buf, 0), "hello") == 0, "middle insert works");
    buffer_destroy(buf);
}

/* ============================================================================
 * buffer_delete_char
 * ============================================================================ */

TEST(test_delete_char_basic)
{
    /* Build "ab", delete 'a' at col 0 — should leave "b". */
    Buffer *buf = buffer_create();
    buffer_insert_char(buf, 0, 0, 'a');
    buffer_insert_char(buf, 0, 1, 'b');
    buffer_delete_char(buf, 0, 0);

    ASSERT(buffer_line_len(buf, 0) == 1,            "length is 1 after delete");
    ASSERT(buffer_get_line(buf, 0)[0] == 'b',       "remaining char is 'b'");
    buffer_destroy(buf);
}

TEST(test_delete_char_last)
{
    /* Delete the only character — line becomes empty. */
    Buffer *buf = buffer_create();
    buffer_insert_char(buf, 0, 0, 'x');
    buffer_delete_char(buf, 0, 0);

    ASSERT(buffer_line_len(buf, 0) == 0,            "line is empty after deleting last char");
    buffer_destroy(buf);
}

/* ============================================================================
 * buffer_insert_newline
 * ============================================================================ */

TEST(test_insert_newline_splits_line)
{
    /*
     * Build "hello" on line 0, then split at col 3.
     * Result: line 0 = "hel", line 1 = "lo".
     */
    Buffer *buf = buffer_create();
    for (int i = 0; "hello"[i]; i++)
        buffer_insert_char(buf, 0, i, "hello"[i]);

    buffer_insert_newline(buf, 0, 3);

    ASSERT(buf->num_lines == 2,                          "2 lines after newline");
    ASSERT(strcmp(buffer_get_line(buf, 0), "hel") == 0, "line 0 is 'hel'");
    ASSERT(strcmp(buffer_get_line(buf, 1), "lo")  == 0, "line 1 is 'lo'");
    buffer_destroy(buf);
}

TEST(test_insert_newline_at_start)
{
    /* Split at col 0 — line 0 becomes empty, line 1 gets the full content. */
    Buffer *buf = buffer_create();
    buffer_insert_char(buf, 0, 0, 'a');
    buffer_insert_char(buf, 0, 1, 'b');
    buffer_insert_newline(buf, 0, 0);

    ASSERT(buf->num_lines == 2,                         "2 lines");
    ASSERT(buffer_line_len(buf, 0) == 0,                "line 0 empty");
    ASSERT(strcmp(buffer_get_line(buf, 1), "ab") == 0,  "line 1 is 'ab'");
    buffer_destroy(buf);
}

TEST(test_insert_newline_at_end)
{
    /* Split at end of line — line 1 becomes empty. */
    Buffer *buf = buffer_create();
    buffer_insert_char(buf, 0, 0, 'a');
    buffer_insert_newline(buf, 0, 1);

    ASSERT(buf->num_lines == 2,                         "2 lines");
    ASSERT(strcmp(buffer_get_line(buf, 0), "a") == 0,   "line 0 is 'a'");
    ASSERT(buffer_line_len(buf, 1) == 0,                "line 1 empty");
    buffer_destroy(buf);
}

/* ============================================================================
 * buffer_join_lines
 * ============================================================================ */

TEST(test_join_lines_basic)
{
    /*
     * Create two lines: "foo" and "bar".
     * After joining line 0 with line 1, result should be "foobar" on one line.
     */
    Buffer *buf = buffer_create();
    for (int i = 0; "foo"[i]; i++)
        buffer_insert_char(buf, 0, i, "foo"[i]);
    buffer_insert_newline(buf, 0, 3);
    for (int i = 0; "bar"[i]; i++)
        buffer_insert_char(buf, 1, i, "bar"[i]);

    buffer_join_lines(buf, 0);

    ASSERT(buf->num_lines == 1,                             "1 line after join");
    ASSERT(strcmp(buffer_get_line(buf, 0), "foobar") == 0, "joined content is 'foobar'");
    buffer_destroy(buf);
}

TEST(test_join_lines_with_empty)
{
    /* Join a line with an empty line — result is just the non-empty content. */
    Buffer *buf = buffer_create();
    buffer_insert_char(buf, 0, 0, 'x');
    buffer_insert_newline(buf, 0, 1);   /* line 1 is empty */

    buffer_join_lines(buf, 0);

    ASSERT(buf->num_lines == 1,                           "1 line after join");
    ASSERT(strcmp(buffer_get_line(buf, 0), "x") == 0,    "content is 'x'");
    buffer_destroy(buf);
}

/* ============================================================================
 * buffer_delete_region
 * ============================================================================ */

TEST(test_delete_region_single_line)
{
    /*
     * Build "hello world" on line 0.
     * Delete the region [col 5, col 11) — removes " world".
     * Result: "hello".
     */
    Buffer *buf = buffer_create();
    const char *s = "hello world";
    for (int i = 0; s[i]; i++)
        buffer_insert_char(buf, 0, i, s[i]);

    buffer_delete_region(buf, 0, 5, 0, 11);

    ASSERT(buf->num_lines == 1,                            "still 1 line");
    ASSERT(strcmp(buffer_get_line(buf, 0), "hello") == 0, "region deleted correctly");
    buffer_destroy(buf);
}

TEST(test_delete_region_multi_line)
{
    /*
     * Build three lines: "aaa", "bbb", "ccc".
     * Delete from line 0 col 1 to line 2 col 2 — removes "aa\nbbb\ncc".
     * Result: one line "ac".
     */
    Buffer *buf = buffer_create();

    /* line 0: "aaa" */
    buffer_insert_char(buf, 0, 0, 'a');
    buffer_insert_char(buf, 0, 1, 'a');
    buffer_insert_char(buf, 0, 2, 'a');
    /* line 1: "bbb" */
    buffer_insert_newline(buf, 0, 3);
    buffer_insert_char(buf, 1, 0, 'b');
    buffer_insert_char(buf, 1, 1, 'b');
    buffer_insert_char(buf, 1, 2, 'b');
    /* line 2: "ccc" */
    buffer_insert_newline(buf, 1, 3);
    buffer_insert_char(buf, 2, 0, 'c');
    buffer_insert_char(buf, 2, 1, 'c');
    buffer_insert_char(buf, 2, 2, 'c');

    buffer_delete_region(buf, 0, 1, 2, 2);

    ASSERT(buf->num_lines == 1,                           "1 line after multi-line delete");
    ASSERT(strcmp(buffer_get_line(buf, 0), "ac") == 0,   "remaining content is 'ac'");
    buffer_destroy(buf);
}

/* ============================================================================
 * Queries
 * ============================================================================ */

TEST(test_line_len_out_of_bounds)
{
    /* buffer_line_len should return -1 for an invalid row. */
    Buffer *buf = buffer_create();
    ASSERT(buffer_line_len(buf, -1) == -1, "negative row returns -1");
    ASSERT(buffer_line_len(buf,  1) == -1, "row past end returns -1");
    buffer_destroy(buf);
}

TEST(test_get_line_out_of_bounds)
{
    /* buffer_get_line should return NULL for an invalid row. */
    Buffer *buf = buffer_create();
    ASSERT(buffer_get_line(buf, 99) == NULL, "out-of-bounds row returns NULL");
    buffer_destroy(buf);
}

/* ============================================================================
 * UTF-8 byte length helper
 *
 * This is a copy of the static utf8_byte_len() from display.c / gui.c so
 * we can unit-test the logic here.  If this function is ever extracted to a
 * shared header, replace this copy with an #include.
 * ============================================================================ */

static int utf8_byte_len(unsigned char c)
{
    if (c < 0x80) return 1;
    if (c < 0xC0) return 1;
    if (c < 0xE0) return 2;
    if (c < 0xF0) return 3;
    return 4;
}

TEST(test_utf8_byte_len_ascii)
{
    /*
     * ASCII bytes (0x00–0x7F) are single-byte characters.
     * Test a few representative values: NUL, space, 'A', DEL.
     */
    ASSERT(utf8_byte_len(0x00) == 1, "NUL is 1 byte");
    ASSERT(utf8_byte_len(' ')  == 1, "space is 1 byte");
    ASSERT(utf8_byte_len('A')  == 1, "'A' is 1 byte");
    ASSERT(utf8_byte_len(0x7F) == 1, "0x7F (DEL) is 1 byte");
}

TEST(test_utf8_byte_len_continuation)
{
    /*
     * Continuation bytes (0x80–0xBF) should never appear as a leading byte.
     * We return 1 so the renderer safely skips past them.
     */
    ASSERT(utf8_byte_len(0x80) == 1, "0x80 continuation → 1");
    ASSERT(utf8_byte_len(0xBF) == 1, "0xBF continuation → 1");
}

TEST(test_utf8_byte_len_two_byte)
{
    /*
     * Two-byte sequences have lead bytes 0xC0–0xDF.
     * Examples: é (C3 A9), ñ (C3 B1), ü (C3 BC).
     */
    ASSERT(utf8_byte_len(0xC0) == 2, "0xC0 → 2 bytes");
    ASSERT(utf8_byte_len(0xC3) == 2, "0xC3 (é lead) → 2 bytes");
    ASSERT(utf8_byte_len(0xDF) == 2, "0xDF → 2 bytes");
}

TEST(test_utf8_byte_len_three_byte)
{
    /*
     * Three-byte sequences have lead bytes 0xE0–0xEF.
     * Examples: € (E2 82 AC), 中 (E4 B8 AD), → (E2 86 92).
     */
    ASSERT(utf8_byte_len(0xE0) == 3, "0xE0 → 3 bytes");
    ASSERT(utf8_byte_len(0xE2) == 3, "0xE2 (€ lead) → 3 bytes");
    ASSERT(utf8_byte_len(0xEF) == 3, "0xEF → 3 bytes");
}

TEST(test_utf8_byte_len_four_byte)
{
    /*
     * Four-byte sequences have lead bytes 0xF0–0xF7.
     * Examples: 😀 (F0 9F 98 80), 𝕳 (F0 9D 95 B3).
     */
    ASSERT(utf8_byte_len(0xF0) == 4, "0xF0 (emoji lead) → 4 bytes");
    ASSERT(utf8_byte_len(0xF4) == 4, "0xF4 → 4 bytes");
    ASSERT(utf8_byte_len(0xF7) == 4, "0xF7 → 4 bytes");
}

/* ============================================================================
 * main
 * ============================================================================ */

int main(void)
{
    printf("=== test_buffer ===\n");

    RUN(test_create_has_one_empty_line);

    RUN(test_insert_char_basic);
    RUN(test_insert_char_builds_string);
    RUN(test_insert_char_middle);

    RUN(test_delete_char_basic);
    RUN(test_delete_char_last);

    RUN(test_insert_newline_splits_line);
    RUN(test_insert_newline_at_start);
    RUN(test_insert_newline_at_end);

    RUN(test_join_lines_basic);
    RUN(test_join_lines_with_empty);

    RUN(test_delete_region_single_line);
    RUN(test_delete_region_multi_line);

    RUN(test_line_len_out_of_bounds);
    RUN(test_get_line_out_of_bounds);

    RUN(test_utf8_byte_len_ascii);
    RUN(test_utf8_byte_len_continuation);
    RUN(test_utf8_byte_len_two_byte);
    RUN(test_utf8_byte_len_three_byte);
    RUN(test_utf8_byte_len_four_byte);

    TEST_SUMMARY();
}
