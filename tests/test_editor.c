/*
 * test_editor.c — Unit tests for editor.c
 * =============================================================================
 * Tests editor-level operations: cursor movement, text insertion/deletion,
 * undo/redo, selection, and the tab/auto-indent smart-editing features.
 *
 * We do NOT test display or input here — those depend on ncurses.
 * editor.c uses GUTTER_WIDTH and TAB_BAR_HEIGHT from display.h but does not
 * call any display_* functions, so we can link editor.o without display.o.
 *
 * Each test calls editor_init() and editor_new_buffer() to get a fresh editor,
 * and sets term_rows/term_cols so that editor_scroll() has valid dimensions.
 * =============================================================================
 */

#include "test_runner.h"
#include "editor.h"
#include "buffer.h"

#include <string.h>   /* strcmp */

/* ============================================================================
 * Helper — fresh editor ready for testing
 * ============================================================================ */

/*
 * make_editor — initialise an Editor and open one empty buffer.
 *
 * We give it a 24×80 virtual terminal so editor_scroll() can do its job
 * without hitting edge cases from zero-size dimensions.
 */
static void make_editor(Editor *ed)
{
    editor_init(ed);
    ed->term_rows = 24;
    ed->term_cols = 80;
    editor_new_buffer(ed);
}

/* Convenience: get a pointer to the current buffer's line text. */
static const char *line(Editor *ed, int row)
{
    return buffer_get_line(editor_current_buffer(ed), row);
}

/* Convenience: get line length. */
static int line_len(Editor *ed, int row)
{
    return buffer_line_len(editor_current_buffer(ed), row);
}

/* Convenience: number of lines in current buffer. */
static int num_lines(Editor *ed)
{
    return editor_current_buffer(ed)->num_lines;
}

/* ============================================================================
 * Lifecycle
 * ============================================================================ */

TEST(test_init_creates_buffer)
{
    Editor ed;
    make_editor(&ed);
    ASSERT(ed.num_buffers == 1,       "one buffer after init");
    ASSERT(ed.cursor_row  == 0,       "cursor starts at row 0");
    ASSERT(ed.cursor_col  == 0,       "cursor starts at col 0");
    ASSERT(num_lines(&ed) == 1,       "buffer has one line");
    editor_cleanup(&ed);
}

/* ============================================================================
 * Insert / delete characters
 * ============================================================================ */

TEST(test_insert_char_advances_cursor)
{
    Editor ed;
    make_editor(&ed);
    editor_insert_char(&ed, 'a');
    ASSERT(ed.cursor_col == 1,                    "cursor advances after insert");
    ASSERT(strcmp(line(&ed, 0), "a") == 0,        "line contains 'a'");
    editor_cleanup(&ed);
}

TEST(test_insert_multiple_chars)
{
    Editor ed;
    make_editor(&ed);
    editor_insert_char(&ed, 'f');
    editor_insert_char(&ed, 'o');
    editor_insert_char(&ed, 'o');
    ASSERT(ed.cursor_col == 3,                      "cursor at col 3");
    ASSERT(strcmp(line(&ed, 0), "foo") == 0,        "line is 'foo'");
    editor_cleanup(&ed);
}

TEST(test_backspace_deletes_char)
{
    Editor ed;
    make_editor(&ed);
    editor_insert_char(&ed, 'a');
    editor_insert_char(&ed, 'b');
    editor_backspace(&ed);   /* removes 'b' */
    ASSERT(ed.cursor_col == 1,                     "cursor back at col 1");
    ASSERT(strcmp(line(&ed, 0), "a") == 0,         "line is 'a'");
    editor_cleanup(&ed);
}

TEST(test_backspace_at_col0_joins_lines)
{
    /*
     * Two lines: "foo" and "bar".
     * Put cursor at start of line 1 and backspace — should join into "foobar".
     */
    Editor ed;
    make_editor(&ed);
    editor_insert_char(&ed, 'f');
    editor_insert_char(&ed, 'o');
    editor_insert_char(&ed, 'o');
    editor_insert_newline(&ed);   /* line 1 created */
    editor_insert_char(&ed, 'b');
    editor_insert_char(&ed, 'a');
    editor_insert_char(&ed, 'r');
    /* Move to start of line 1 */
    editor_move_line_start(&ed);
    editor_backspace(&ed);   /* joins with line 0 */

    ASSERT(num_lines(&ed) == 1,                          "1 line after join");
    ASSERT(strcmp(line(&ed, 0), "foobar") == 0,          "joined content");
    ASSERT(ed.cursor_row == 0,                           "cursor on row 0");
    ASSERT(ed.cursor_col == 3,                           "cursor at join point");
    editor_cleanup(&ed);
}

TEST(test_delete_char_removes_under_cursor)
{
    Editor ed;
    make_editor(&ed);
    editor_insert_char(&ed, 'a');
    editor_insert_char(&ed, 'b');
    editor_move_line_start(&ed);   /* cursor at col 0 */
    editor_delete_char(&ed);       /* deletes 'a' */
    ASSERT(strcmp(line(&ed, 0), "b") == 0,  "delete_char removes char under cursor");
    ASSERT(ed.cursor_col == 0,              "cursor stays at col 0");
    editor_cleanup(&ed);
}

/* ============================================================================
 * Newline / auto-indent
 * ============================================================================ */

TEST(test_insert_newline_splits)
{
    Editor ed;
    make_editor(&ed);
    editor_insert_char(&ed, 'a');
    editor_insert_char(&ed, 'b');
    editor_move_line_start(&ed);
    editor_move_right(&ed);   /* cursor at col 1, between 'a' and 'b' */
    editor_insert_newline(&ed);

    ASSERT(num_lines(&ed) == 2,                        "2 lines after newline");
    ASSERT(strcmp(line(&ed, 0), "a") == 0,             "line 0 is 'a'");
    ASSERT(strcmp(line(&ed, 1), "b") == 0,             "line 1 is 'b'");
    ASSERT(ed.cursor_row == 1,                         "cursor on line 1");
    editor_cleanup(&ed);
}

TEST(test_auto_indent_copies_leading_spaces)
{
    /*
     * Build a line with 2 leading spaces ("  hello"), then press Enter.
     * The new line should start with 2 spaces automatically.
     */
    Editor ed;
    make_editor(&ed);
    editor_insert_char(&ed, ' ');
    editor_insert_char(&ed, ' ');
    editor_insert_char(&ed, 'h');
    editor_insert_char(&ed, 'i');
    editor_insert_newline(&ed);

    ASSERT(ed.cursor_row == 1,                   "cursor on new line");
    ASSERT(ed.cursor_col == 2,                   "cursor after 2-space indent");
    ASSERT(line(&ed, 1)[0] == ' ',               "new line starts with space");
    ASSERT(line(&ed, 1)[1] == ' ',               "second char is also space");
    editor_cleanup(&ed);
}

TEST(test_auto_indent_no_indent_for_empty_line)
{
    /* A line with no leading whitespace should produce no indent. */
    Editor ed;
    make_editor(&ed);
    editor_insert_char(&ed, 'x');
    editor_insert_newline(&ed);

    ASSERT(ed.cursor_col == 0,              "cursor at col 0 — no indent");
    ASSERT(line_len(&ed, 1) == 0,           "new line is empty");
    editor_cleanup(&ed);
}

/* ============================================================================
 * Tab insert
 * ============================================================================ */

TEST(test_insert_tab_inserts_spaces)
{
    Editor ed;
    make_editor(&ed);
    editor_insert_tab(&ed);

    /* Default tab_width is 4 */
    ASSERT(ed.cursor_col == 4,              "cursor at col 4 after tab");
    ASSERT(line_len(&ed, 0) == 4,           "line length is 4");
    ASSERT(line(&ed, 0)[0] == ' ',          "inserted spaces, not tab char");
    editor_cleanup(&ed);
}

/* ============================================================================
 * Cursor movement
 * ============================================================================ */

TEST(test_move_left_right)
{
    Editor ed;
    make_editor(&ed);
    editor_insert_char(&ed, 'a');
    editor_insert_char(&ed, 'b');
    /* cursor is at col 2 */
    editor_move_left(&ed);
    ASSERT(ed.cursor_col == 1, "move_left decrements col");
    editor_move_right(&ed);
    ASSERT(ed.cursor_col == 2, "move_right increments col");
    editor_cleanup(&ed);
}

TEST(test_move_left_wraps_to_prev_line)
{
    /* Moving left at col 0 of line 1 should move to end of line 0. */
    Editor ed;
    make_editor(&ed);
    editor_insert_char(&ed, 'a');
    editor_insert_newline(&ed);
    /* cursor is at row 1 col 0 */
    editor_move_left(&ed);
    ASSERT(ed.cursor_row == 0,       "cursor moved to row 0");
    ASSERT(ed.cursor_col == 1,       "cursor at end of line 0");
    editor_cleanup(&ed);
}

TEST(test_move_up_down)
{
    Editor ed;
    make_editor(&ed);
    editor_insert_char(&ed, 'a');
    editor_insert_newline(&ed);
    editor_insert_char(&ed, 'b');
    /* cursor on row 1 */
    editor_move_up(&ed);
    ASSERT(ed.cursor_row == 0, "move_up goes to row 0");
    editor_move_down(&ed);
    ASSERT(ed.cursor_row == 1, "move_down goes back to row 1");
    editor_cleanup(&ed);
}

TEST(test_move_line_start_end)
{
    Editor ed;
    make_editor(&ed);
    editor_insert_char(&ed, 'a');
    editor_insert_char(&ed, 'b');
    editor_insert_char(&ed, 'c');
    editor_move_line_start(&ed);
    ASSERT(ed.cursor_col == 0, "move_line_start goes to col 0");
    editor_move_line_end(&ed);
    ASSERT(ed.cursor_col == 3, "move_line_end goes to end");
    editor_cleanup(&ed);
}

TEST(test_move_clamps_at_boundaries)
{
    Editor ed;
    make_editor(&ed);
    /* Moving up from row 0 should stay at row 0 */
    editor_move_up(&ed);
    ASSERT(ed.cursor_row == 0, "move_up clamped at row 0");
    /* Moving left from col 0 on the only line should stay at col 0 */
    editor_move_left(&ed);
    ASSERT(ed.cursor_col == 0, "move_left clamped at col 0 on first line");
    editor_cleanup(&ed);
}

/* ============================================================================
 * Undo / redo
 * ============================================================================ */

TEST(test_undo_insert_char)
{
    Editor ed;
    make_editor(&ed);
    editor_insert_char(&ed, 'x');
    editor_undo(&ed);
    ASSERT(line_len(&ed, 0) == 0,    "undo removes inserted char");
    ASSERT(ed.cursor_col == 0,       "cursor back at col 0");
    editor_cleanup(&ed);
}

TEST(test_redo_insert_char)
{
    Editor ed;
    make_editor(&ed);
    editor_insert_char(&ed, 'x');
    editor_undo(&ed);
    editor_redo(&ed);
    ASSERT(line_len(&ed, 0) == 1,    "redo re-inserts char");
    ASSERT(ed.cursor_col == 1,       "cursor at col 1 after redo");
    editor_cleanup(&ed);
}

TEST(test_undo_newline)
{
    Editor ed;
    make_editor(&ed);
    editor_insert_char(&ed, 'a');
    editor_insert_newline(&ed);
    ASSERT(num_lines(&ed) == 2,      "2 lines before undo");
    editor_undo(&ed);
    ASSERT(num_lines(&ed) == 1,      "1 line after undo of newline");
    ASSERT(strcmp(line(&ed, 0), "a") == 0, "content restored");
    editor_cleanup(&ed);
}

TEST(test_undo_tab)
{
    /* Tab inserts as a single UNDO_PASTE — one Ctrl+Z removes all 4 spaces. */
    Editor ed;
    make_editor(&ed);
    editor_insert_tab(&ed);
    ASSERT(line_len(&ed, 0) == 4,    "4 spaces inserted");
    editor_undo(&ed);
    ASSERT(line_len(&ed, 0) == 0,    "undo removes all 4 spaces at once");
    editor_cleanup(&ed);
}

TEST(test_nothing_to_undo_is_safe)
{
    /* Calling undo on an empty history should not crash. */
    Editor ed;
    make_editor(&ed);
    editor_undo(&ed);   /* should show status message, not crash */
    ASSERT(1, "undo on empty history did not crash");
    editor_cleanup(&ed);
}

/* ============================================================================
 * Select all
 * ============================================================================ */

TEST(test_select_all)
{
    Editor ed;
    make_editor(&ed);
    editor_insert_char(&ed, 'a');
    editor_insert_newline(&ed);
    editor_insert_char(&ed, 'b');
    editor_select_all(&ed);

    ASSERT(ed.sel_active,              "selection is active");
    ASSERT(ed.sel_anchor_row == 0,     "anchor at row 0");
    ASSERT(ed.sel_anchor_col == 0,     "anchor at col 0");
    ASSERT(ed.cursor_row == 1,         "cursor at last row");
    ASSERT(ed.cursor_col == 1,         "cursor at end of last line");
    editor_cleanup(&ed);
}

/* ============================================================================
 * main
 * ============================================================================ */

int main(void)
{
    printf("=== test_editor ===\n");

    RUN(test_init_creates_buffer);

    RUN(test_insert_char_advances_cursor);
    RUN(test_insert_multiple_chars);
    RUN(test_backspace_deletes_char);
    RUN(test_backspace_at_col0_joins_lines);
    RUN(test_delete_char_removes_under_cursor);

    RUN(test_insert_newline_splits);
    RUN(test_auto_indent_copies_leading_spaces);
    RUN(test_auto_indent_no_indent_for_empty_line);

    RUN(test_insert_tab_inserts_spaces);

    RUN(test_move_left_right);
    RUN(test_move_left_wraps_to_prev_line);
    RUN(test_move_up_down);
    RUN(test_move_line_start_end);
    RUN(test_move_clamps_at_boundaries);

    RUN(test_undo_insert_char);
    RUN(test_redo_insert_char);
    RUN(test_undo_newline);
    RUN(test_undo_tab);
    RUN(test_nothing_to_undo_is_safe);

    RUN(test_select_all);

    TEST_SUMMARY();
}
