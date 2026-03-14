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
 * Jump to line
 * ============================================================================ */

/*
 * editor_goto_line calls display_prompt, which returns NULL in the test stub
 * (simulating Escape).  So we test the underlying move logic directly by
 * setting cursor_row and calling editor_scroll, the same way editor_goto_line
 * does internally.  We verify the line-clamping behaviour via the buffer API.
 */
TEST(test_goto_line_moves_cursor)
{
    Editor ed;
    make_editor(&ed);

    /* Build 5 lines */
    for (int i = 0; i < 4; i++) {
        editor_insert_char(&ed, 'x');
        editor_insert_newline(&ed);
    }
    ASSERT(num_lines(&ed) == 5, "5 lines set up");

    /* Jump to line 3 (0-based row 2) */
    Buffer *buf = editor_current_buffer(&ed);
    int row = 2;   /* 1-based line 3 */
    if (row >= buf->num_lines) row = buf->num_lines - 1;
    ed.cursor_row  = row;
    ed.cursor_col  = 0;
    ed.desired_col = 0;
    editor_selection_clear(&ed);
    editor_scroll(&ed);

    ASSERT(ed.cursor_row == 2, "cursor moved to row 2 (line 3)");
    ASSERT(ed.cursor_col == 0, "cursor at col 0");
    editor_cleanup(&ed);
}

TEST(test_goto_line_clamps_to_last)
{
    Editor ed;
    make_editor(&ed);

    /* Only 1 line exists; clamping a large row should land on row 0 */
    Buffer *buf = editor_current_buffer(&ed);
    int row = 999;
    if (row >= buf->num_lines) row = buf->num_lines - 1;
    ed.cursor_row  = row;
    ed.cursor_col  = 0;
    ed.desired_col = 0;
    editor_scroll(&ed);

    ASSERT(ed.cursor_row == 0, "out-of-range line clamped to last row");
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
 * Bracket matching
 * ============================================================================ */

/*
 * Helper: build a buffer with the given text on line 0, place the cursor at
 * `col`, then call editor_find_bracket_match and return whether it found a
 * match.  If it did, writes the match position to out_row/out_col.
 */
static int bracket_match(Editor *ed, const char *text, int col,
                          int *mr, int *mc)
{
    /* Insert text character by character */
    for (int i = 0; text[i]; i++)
        editor_insert_char(ed, text[i]);
    /* Move cursor to the requested column */
    ed->cursor_row  = 0;
    ed->cursor_col  = col;
    ed->desired_col = col;
    return editor_find_bracket_match(ed, mr, mc);
}

TEST(test_bracket_match_open_paren)
{
    /* "()" — cursor on '(' at col 0, match should be ')' at col 1 */
    Editor ed; make_editor(&ed);
    int mr, mc;
    int found = bracket_match(&ed, "()", 0, &mr, &mc);
    ASSERT(found == 1,  "match found for '('");
    ASSERT(mr == 0,     "match on row 0");
    ASSERT(mc == 1,     "match at col 1");
    editor_cleanup(&ed);
}

TEST(test_bracket_match_close_paren)
{
    /* "()" — cursor on ')' at col 1, match should be '(' at col 0 */
    Editor ed; make_editor(&ed);
    int mr, mc;
    int found = bracket_match(&ed, "()", 1, &mr, &mc);
    ASSERT(found == 1,  "match found for ')'");
    ASSERT(mr == 0,     "match on row 0");
    ASSERT(mc == 0,     "match at col 0");
    editor_cleanup(&ed);
}

TEST(test_bracket_match_square)
{
    /* "[]" — cursor on '[' */
    Editor ed; make_editor(&ed);
    int mr, mc;
    int found = bracket_match(&ed, "[]", 0, &mr, &mc);
    ASSERT(found == 1,  "match found for '['");
    ASSERT(mc == 1,     "']' at col 1");
    editor_cleanup(&ed);
}

TEST(test_bracket_match_curly)
{
    /* "{}" — cursor on '{' */
    Editor ed; make_editor(&ed);
    int mr, mc;
    int found = bracket_match(&ed, "{}", 0, &mr, &mc);
    ASSERT(found == 1,  "match found for '{'");
    ASSERT(mc == 1,     "'}' at col 1");
    editor_cleanup(&ed);
}

TEST(test_bracket_match_nested)
{
    /* "((()))" — cursor on outer '(' at col 0, match is ')' at col 5 */
    Editor ed; make_editor(&ed);
    int mr, mc;
    int found = bracket_match(&ed, "((()))", 0, &mr, &mc);
    ASSERT(found == 1,  "match found in nested brackets");
    ASSERT(mc == 5,     "outer ')' at col 5");
    editor_cleanup(&ed);
}

TEST(test_bracket_match_inner_nested)
{
    /* "((()))" — cursor on inner '(' at col 1, match is ')' at col 4 */
    Editor ed; make_editor(&ed);
    int mr, mc;
    int found = bracket_match(&ed, "((()))", 1, &mr, &mc);
    ASSERT(found == 1,  "inner match found");
    ASSERT(mc == 4,     "inner ')' at col 4");
    editor_cleanup(&ed);
}

TEST(test_bracket_match_multiline)
{
    /*
     * Line 0: "("
     * Line 1: ")"
     * Cursor on '(' at (0,0), match should be ')' at (1,0).
     */
    Editor ed; make_editor(&ed);
    editor_insert_char(&ed, '(');
    editor_insert_newline(&ed);
    editor_insert_char(&ed, ')');
    ed.cursor_row = 0;
    ed.cursor_col = 0;
    int mr, mc;
    int found = editor_find_bracket_match(&ed, &mr, &mc);
    ASSERT(found == 1,  "multiline match found");
    ASSERT(mr == 1,     "match on row 1");
    ASSERT(mc == 0,     "match at col 0");
    editor_cleanup(&ed);
}

TEST(test_bracket_match_not_a_bracket)
{
    /* Cursor on a non-bracket character — no match */
    Editor ed; make_editor(&ed);
    editor_insert_char(&ed, 'x');
    ed.cursor_row = 0;
    ed.cursor_col = 0;
    int mr, mc;
    int found = editor_find_bracket_match(&ed, &mr, &mc);
    ASSERT(found == 0, "no match for non-bracket character");
    editor_cleanup(&ed);
}

TEST(test_bracket_match_unmatched)
{
    /* "(" with no closing ')' — no match */
    Editor ed; make_editor(&ed);
    editor_insert_char(&ed, '(');
    ed.cursor_row = 0;
    ed.cursor_col = 0;
    int mr, mc;
    int found = editor_find_bracket_match(&ed, &mr, &mc);
    ASSERT(found == 0, "no match for unmatched bracket");
    editor_cleanup(&ed);
}

TEST(test_bracket_match_cursor_past_eol)
{
    /* Cursor is past end of line — no match */
    Editor ed; make_editor(&ed);
    editor_insert_char(&ed, '(');
    ed.cursor_row = 0;
    ed.cursor_col = 5;   /* past end of 1-char line */
    int mr, mc;
    int found = editor_find_bracket_match(&ed, &mr, &mc);
    ASSERT(found == 0, "no match when cursor past EOL");
    editor_cleanup(&ed);
}

/* ============================================================================
 * Auto-close brackets and quotes
 * ============================================================================ */

TEST(test_autoclose_paren)
{
    /*
     * Calling editor_insert_pair directly (simulating the '(' key press).
     * Should produce "()" with the cursor between the two characters.
     */
    Editor ed; make_editor(&ed);
    editor_insert_pair(&ed, '(', ')');
    ASSERT(strcmp(line(&ed, 0), "()") == 0,  "paren pair inserted");
    ASSERT(ed.cursor_col == 1,               "cursor between the pair");
    editor_cleanup(&ed);
}

TEST(test_autoclose_square)
{
    Editor ed; make_editor(&ed);
    editor_insert_pair(&ed, '[', ']');
    ASSERT(strcmp(line(&ed, 0), "[]") == 0,  "square bracket pair inserted");
    ASSERT(ed.cursor_col == 1,               "cursor between the pair");
    editor_cleanup(&ed);
}

TEST(test_autoclose_curly)
{
    Editor ed; make_editor(&ed);
    editor_insert_pair(&ed, '{', '}');
    ASSERT(strcmp(line(&ed, 0), "{}") == 0,  "curly brace pair inserted");
    ASSERT(ed.cursor_col == 1,               "cursor between the pair");
    editor_cleanup(&ed);
}

TEST(test_autoclose_double_quote)
{
    Editor ed; make_editor(&ed);
    editor_insert_pair(&ed, '"', '"');
    ASSERT(strcmp(line(&ed, 0), "\"\"") == 0, "double-quote pair inserted");
    ASSERT(ed.cursor_col == 1,                "cursor between the pair");
    editor_cleanup(&ed);
}

TEST(test_autoclose_single_quote)
{
    Editor ed; make_editor(&ed);
    editor_insert_pair(&ed, '\'', '\'');
    ASSERT(strcmp(line(&ed, 0), "''") == 0,  "single-quote pair inserted");
    ASSERT(ed.cursor_col == 1,               "cursor between the pair");
    editor_cleanup(&ed);
}

TEST(test_autoclose_undo_removes_both)
{
    /*
     * After inserting "()", one Ctrl+Z should remove both characters,
     * leaving the buffer empty and the cursor back at col 0.
     */
    Editor ed; make_editor(&ed);
    editor_insert_pair(&ed, '(', ')');
    editor_undo(&ed);
    ASSERT(line_len(&ed, 0) == 0,  "undo removes both auto-close chars");
    ASSERT(ed.cursor_col == 0,     "cursor back at col 0 after undo");
    editor_cleanup(&ed);
}

TEST(test_autoclose_cursor_mid_line)
{
    /*
     * Insert "ab", move cursor to col 1, then insert a pair.
     * Result should be "a()b" with cursor at col 2 (between the pair).
     */
    Editor ed; make_editor(&ed);
    editor_insert_char(&ed, 'a');
    editor_insert_char(&ed, 'b');
    ed.cursor_col = 1;   /* position between 'a' and 'b' */
    editor_insert_pair(&ed, '(', ')');
    ASSERT(strcmp(line(&ed, 0), "a()b") == 0,  "pair inserted mid-line");
    ASSERT(ed.cursor_col == 2,                  "cursor between the pair");
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

    RUN(test_goto_line_moves_cursor);
    RUN(test_goto_line_clamps_to_last);

    RUN(test_bracket_match_open_paren);
    RUN(test_bracket_match_close_paren);
    RUN(test_bracket_match_square);
    RUN(test_bracket_match_curly);
    RUN(test_bracket_match_nested);
    RUN(test_bracket_match_inner_nested);
    RUN(test_bracket_match_multiline);
    RUN(test_bracket_match_not_a_bracket);
    RUN(test_bracket_match_unmatched);
    RUN(test_bracket_match_cursor_past_eol);

    RUN(test_select_all);

    RUN(test_autoclose_paren);
    RUN(test_autoclose_square);
    RUN(test_autoclose_curly);
    RUN(test_autoclose_double_quote);
    RUN(test_autoclose_single_quote);
    RUN(test_autoclose_undo_removes_both);
    RUN(test_autoclose_cursor_mid_line);

    TEST_SUMMARY();
}
