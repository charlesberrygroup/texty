/*
 * editor.c — Editor State and Operations
 * =============================================================================
 * Implements the functions declared in editor.h.
 *
 * This module knows about the Editor and Buffer structures, but it does NOT
 * know anything about ncurses or terminal rendering — that lives in display.c.
 * =============================================================================
 */

#include "editor.h"
#include "buffer.h"
#include "display.h"    /* for GUTTER_WIDTH */
#include "undo.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

/* ============================================================================
 * Internal helpers
 * ============================================================================ */

/*
 * clamp — return `value` clamped to the range [lo, hi].
 */
static int clamp(int value, int lo, int hi)
{
    if (value < lo) return lo;
    if (value > hi) return hi;
    return value;
}

/*
 * editor_rows — how many rows are available for text content.
 *
 * We reserve 1 row at the bottom for the status bar.
 */
static int editor_rows(const Editor *ed)
{
    return ed->term_rows - 1;
}

/*
 * editor_cols — how many columns are available for text content.
 *
 * We subtract the gutter width (line numbers) from the total terminal width.
 */
static int editor_cols(const Editor *ed)
{
    return ed->term_cols - GUTTER_WIDTH;
}

/* ============================================================================
 * Lifecycle
 * ============================================================================ */

void editor_init(Editor *ed)
{
    memset(ed, 0, sizeof(Editor));
    /* editor_new_buffer will be called by the caller (main.c) */
}

void editor_cleanup(Editor *ed)
{
    for (int i = 0; i < ed->num_buffers; i++) {
        if (ed->buffers[i]) {
            buffer_destroy(ed->buffers[i]);
            ed->buffers[i] = NULL;
        }
    }
    ed->num_buffers    = 0;
    ed->current_buffer = 0;
}

/* ============================================================================
 * Buffer management
 * ============================================================================ */

int editor_new_buffer(Editor *ed)
{
    if (ed->num_buffers >= EDITOR_MAX_BUFFERS) {
        editor_set_status(ed, "Error: too many open buffers (max %d)",
                          EDITOR_MAX_BUFFERS);
        return -1;
    }

    Buffer *buf = buffer_create();
    if (!buf) {
        editor_set_status(ed, "Error: out of memory");
        return -1;
    }

    int idx = ed->num_buffers;
    ed->buffers[idx] = buf;
    ed->num_buffers++;
    ed->current_buffer = idx;

    /* Reset cursor and viewport for the new buffer */
    ed->cursor_row  = 0;
    ed->cursor_col  = 0;
    ed->desired_col = 0;
    ed->view_row    = 0;
    ed->view_col    = 0;

    return 0;
}

int editor_open_file(Editor *ed, const char *path)
{
    if (ed->num_buffers >= EDITOR_MAX_BUFFERS) {
        editor_set_status(ed, "Error: too many open buffers");
        return -1;
    }

    Buffer *buf = buffer_create();
    if (!buf) {
        editor_set_status(ed, "Error: out of memory");
        return -1;
    }

    if (buffer_load_file(buf, path) != 0) {
        /* File might not exist yet — treat as a new file with the given name */
        free(buf->filename);
        buf->filename = strdup(path);
        /* Leave the buffer with one empty line (from buffer_create) */
    }

    int idx = ed->num_buffers;
    ed->buffers[idx] = buf;
    ed->num_buffers++;
    ed->current_buffer = idx;

    /* Reset cursor and viewport */
    ed->cursor_row  = 0;
    ed->cursor_col  = 0;
    ed->desired_col = 0;
    ed->view_row    = 0;
    ed->view_col    = 0;

    return 0;
}

int editor_save(Editor *ed)
{
    Buffer *buf = editor_current_buffer(ed);
    if (!buf) return -1;

    if (!buf->filename) {
        editor_set_status(ed, "No filename — use Ctrl+O to save as (Phase 2)");
        return -1;
    }

    if (buffer_save_file(buf) != 0) {
        editor_set_status(ed, "Error saving \"%s\"", buf->filename);
        return -1;
    }

    editor_set_status(ed, "Saved \"%s\"", buf->filename);
    return 0;
}

Buffer *editor_current_buffer(Editor *ed)
{
    if (ed->num_buffers == 0) return NULL;
    return ed->buffers[ed->current_buffer];
}

/* ============================================================================
 * Cursor movement
 * ============================================================================ */

void editor_move_up(Editor *ed)
{
    Buffer *buf = editor_current_buffer(ed);
    if (!buf || ed->cursor_row == 0) return;

    ed->cursor_row--;
    /* Clamp cursor_col to the new line's length, but remember desired_col */
    int line_len = buffer_line_len(buf, ed->cursor_row);
    ed->cursor_col = clamp(ed->desired_col, 0, line_len);

    editor_scroll(ed);
}

void editor_move_down(Editor *ed)
{
    Buffer *buf = editor_current_buffer(ed);
    if (!buf || ed->cursor_row >= buf->num_lines - 1) return;

    ed->cursor_row++;
    int line_len = buffer_line_len(buf, ed->cursor_row);
    ed->cursor_col = clamp(ed->desired_col, 0, line_len);

    editor_scroll(ed);
}

void editor_move_left(Editor *ed)
{
    Buffer *buf = editor_current_buffer(ed);
    if (!buf) return;

    if (ed->cursor_col > 0) {
        ed->cursor_col--;
    } else if (ed->cursor_row > 0) {
        /* Wrap to end of the previous line */
        ed->cursor_row--;
        ed->cursor_col = buffer_line_len(buf, ed->cursor_row);
    }

    ed->desired_col = ed->cursor_col;
    editor_scroll(ed);
}

void editor_move_right(Editor *ed)
{
    Buffer *buf = editor_current_buffer(ed);
    if (!buf) return;

    int line_len = buffer_line_len(buf, ed->cursor_row);

    if (ed->cursor_col < line_len) {
        ed->cursor_col++;
    } else if (ed->cursor_row < buf->num_lines - 1) {
        /* Wrap to start of the next line */
        ed->cursor_row++;
        ed->cursor_col = 0;
    }

    ed->desired_col = ed->cursor_col;
    editor_scroll(ed);
}

void editor_move_line_start(Editor *ed)
{
    ed->cursor_col  = 0;
    ed->desired_col = 0;
    editor_scroll(ed);
}

void editor_move_line_end(Editor *ed)
{
    Buffer *buf = editor_current_buffer(ed);
    if (!buf) return;

    int line_len    = buffer_line_len(buf, ed->cursor_row);
    ed->cursor_col  = line_len;
    ed->desired_col = line_len;
    editor_scroll(ed);
}

void editor_page_up(Editor *ed)
{
    Buffer *buf = editor_current_buffer(ed);
    if (!buf) return;

    int rows = editor_rows(ed);

    /* Move the cursor up by one screenful */
    ed->cursor_row -= rows;
    if (ed->cursor_row < 0) ed->cursor_row = 0;

    /* Also scroll the viewport */
    ed->view_row -= rows;
    if (ed->view_row < 0) ed->view_row = 0;

    int line_len   = buffer_line_len(buf, ed->cursor_row);
    ed->cursor_col = clamp(ed->desired_col, 0, line_len);

    editor_scroll(ed);
}

void editor_page_down(Editor *ed)
{
    Buffer *buf = editor_current_buffer(ed);
    if (!buf) return;

    int rows       = editor_rows(ed);
    int last_line  = buf->num_lines - 1;

    ed->cursor_row += rows;
    if (ed->cursor_row > last_line) ed->cursor_row = last_line;

    ed->view_row += rows;
    /* editor_scroll will clamp view_row correctly */

    int line_len   = buffer_line_len(buf, ed->cursor_row);
    ed->cursor_col = clamp(ed->desired_col, 0, line_len);

    editor_scroll(ed);
}

void editor_move_file_start(Editor *ed)
{
    ed->cursor_row  = 0;
    ed->cursor_col  = 0;
    ed->desired_col = 0;
    editor_scroll(ed);
}

void editor_move_file_end(Editor *ed)
{
    Buffer *buf = editor_current_buffer(ed);
    if (!buf) return;

    ed->cursor_row  = buf->num_lines - 1;
    int line_len    = buffer_line_len(buf, ed->cursor_row);
    ed->cursor_col  = line_len;
    ed->desired_col = line_len;
    editor_scroll(ed);
}

/* ============================================================================
 * Text editing
 * ============================================================================ */

void editor_insert_char(Editor *ed, char c)
{
    Buffer *buf = editor_current_buffer(ed);
    if (!buf) return;

    /*
     * Record the undo entry BEFORE making the change.
     * We store the cursor position before (for undo) and the expected
     * cursor position after (cursor_col + 1, for redo).
     */
    UndoRecord rec;
    rec.type              = UNDO_INSERT_CHAR;
    rec.row               = ed->cursor_row;
    rec.col               = ed->cursor_col;
    rec.c                 = c;
    rec.cursor_row_before = ed->cursor_row;
    rec.cursor_col_before = ed->cursor_col;
    rec.cursor_row_after  = ed->cursor_row;
    rec.cursor_col_after  = ed->cursor_col + 1;
    undo_push(&buf->undo_stack, rec);

    /* Any new edit invalidates the redo history */
    undo_clear(&buf->redo_stack);

    buffer_insert_char(buf, ed->cursor_row, ed->cursor_col, c);

    ed->cursor_col++;
    ed->desired_col = ed->cursor_col;
    editor_scroll(ed);
}

void editor_insert_newline(Editor *ed)
{
    Buffer *buf = editor_current_buffer(ed);
    if (!buf) return;

    /*
     * Record the split point.  To undo, we call join_lines(row), which
     * joins line `row` with line `row+1` — exactly what Enter split apart.
     */
    UndoRecord rec;
    rec.type              = UNDO_INSERT_NEWLINE;
    rec.row               = ed->cursor_row;
    rec.col               = ed->cursor_col;
    rec.c                 = 0;               /* not used for newline */
    rec.cursor_row_before = ed->cursor_row;
    rec.cursor_col_before = ed->cursor_col;
    rec.cursor_row_after  = ed->cursor_row + 1;
    rec.cursor_col_after  = 0;
    undo_push(&buf->undo_stack, rec);

    undo_clear(&buf->redo_stack);

    buffer_insert_newline(buf, ed->cursor_row, ed->cursor_col);

    ed->cursor_row++;
    ed->cursor_col  = 0;
    ed->desired_col = 0;
    editor_scroll(ed);
}

void editor_backspace(Editor *ed)
{
    Buffer *buf = editor_current_buffer(ed);
    if (!buf) return;

    UndoRecord rec;

    if (ed->cursor_col > 0) {
        /*
         * Delete the character immediately to the LEFT of the cursor.
         * We must read the character BEFORE deleting it — afterwards it's gone.
         *
         * buffer_get_line returns a pointer to the line's text array.
         * Indexing with [cursor_col - 1] gives us the character to delete.
         */
        int  del_col = ed->cursor_col - 1;
        char del_c   = buffer_get_line(buf, ed->cursor_row)[del_col];

        rec.type              = UNDO_DELETE_CHAR;
        rec.row               = ed->cursor_row;
        rec.col               = del_col;
        rec.c                 = del_c;
        rec.cursor_row_before = ed->cursor_row;
        rec.cursor_col_before = ed->cursor_col;
        rec.cursor_row_after  = ed->cursor_row;
        rec.cursor_col_after  = del_col;
        undo_push(&buf->undo_stack, rec);
        undo_clear(&buf->redo_stack);

        buffer_delete_char(buf, ed->cursor_row, del_col);
        ed->cursor_col--;

    } else if (ed->cursor_row > 0) {
        /*
         * At column 0 — pressing Backspace joins this line with the one above.
         * The join point is the current length of the line above (that's where
         * the newline character effectively was).
         *
         * To undo a join, we call insert_newline(row-1, col=prev_line_len),
         * which splits line (row-1) at the join point, restoring both lines.
         */
        int join_row = ed->cursor_row - 1;
        int join_col = buffer_line_len(buf, join_row); /* length of line above */

        rec.type              = UNDO_JOIN_LINES;
        rec.row               = join_row;
        rec.col               = join_col;
        rec.c                 = 0;
        rec.cursor_row_before = ed->cursor_row;
        rec.cursor_col_before = 0;
        rec.cursor_row_after  = join_row;
        rec.cursor_col_after  = join_col;
        undo_push(&buf->undo_stack, rec);
        undo_clear(&buf->redo_stack);

        buffer_join_lines(buf, join_row);
        ed->cursor_row--;
        ed->cursor_col = join_col;
    }

    ed->desired_col = ed->cursor_col;
    editor_scroll(ed);
}

void editor_delete_char(Editor *ed)
{
    Buffer *buf = editor_current_buffer(ed);
    if (!buf) return;

    int line_len = buffer_line_len(buf, ed->cursor_row);
    UndoRecord rec;

    if (ed->cursor_col < line_len) {
        /* Delete the character UNDER the cursor (at cursor_col) */
        char del_c = buffer_get_line(buf, ed->cursor_row)[ed->cursor_col];

        rec.type              = UNDO_DELETE_CHAR;
        rec.row               = ed->cursor_row;
        rec.col               = ed->cursor_col;
        rec.c                 = del_c;
        rec.cursor_row_before = ed->cursor_row;
        rec.cursor_col_before = ed->cursor_col;
        rec.cursor_row_after  = ed->cursor_row;
        rec.cursor_col_after  = ed->cursor_col;  /* cursor stays put */
        undo_push(&buf->undo_stack, rec);
        undo_clear(&buf->redo_stack);

        buffer_delete_char(buf, ed->cursor_row, ed->cursor_col);

    } else if (ed->cursor_row < buf->num_lines - 1) {
        /*
         * At end of line — Delete joins this line with the one below.
         * join_col is the current line length (= the join point).
         */
        int join_col = line_len;

        rec.type              = UNDO_JOIN_LINES;
        rec.row               = ed->cursor_row;
        rec.col               = join_col;
        rec.c                 = 0;
        rec.cursor_row_before = ed->cursor_row;
        rec.cursor_col_before = ed->cursor_col;
        rec.cursor_row_after  = ed->cursor_row;
        rec.cursor_col_after  = ed->cursor_col;
        undo_push(&buf->undo_stack, rec);
        undo_clear(&buf->redo_stack);

        buffer_join_lines(buf, ed->cursor_row);
    }
    /* If on the last line at end-of-line, nothing to delete */

    ed->desired_col = ed->cursor_col;
    editor_scroll(ed);
}

/* ============================================================================
 * Undo / Redo
 * ============================================================================ */

void editor_undo(Editor *ed)
{
    Buffer *buf = editor_current_buffer(ed);
    if (!buf) return;

    UndoRecord rec;
    if (!undo_pop(&buf->undo_stack, &rec)) {
        editor_set_status(ed, "Nothing to undo.");
        return;
    }

    /*
     * Reverse the recorded operation using the buffer's low-level functions
     * directly.  We bypass the editor_* wrappers to avoid accidentally
     * recording a new undo entry for the reversal itself.
     *
     * After reversing, restore the cursor to where it was BEFORE the original
     * edit, then push this record to the redo stack so the user can redo it.
     */
    switch (rec.type) {

        case UNDO_INSERT_CHAR:
            /*
             * Undo an insert: delete the character that was inserted.
             * It is at (rec.row, rec.col) — exactly where it was put.
             */
            buffer_delete_char(buf, rec.row, rec.col);
            break;

        case UNDO_DELETE_CHAR:
            /*
             * Undo a delete: re-insert the character that was removed.
             * `rec.c` holds the deleted character; `rec.row/col` is
             * where it lived.
             */
            buffer_insert_char(buf, rec.row, rec.col, rec.c);
            break;

        case UNDO_INSERT_NEWLINE:
            /*
             * Undo an Enter press: re-join the two lines that were split.
             * join_lines(row) merges line `row` with line `row+1`,
             * which is exactly what the Enter split.
             */
            buffer_join_lines(buf, rec.row);
            break;

        case UNDO_JOIN_LINES:
            /*
             * Undo a line join: re-split the merged line at the join point.
             * insert_newline(row, col) splits line `row` at column `col`.
             */
            buffer_insert_newline(buf, rec.row, rec.col);
            break;
    }

    /* Restore cursor to where it was before the original edit */
    ed->cursor_row  = rec.cursor_row_before;
    ed->cursor_col  = rec.cursor_col_before;
    ed->desired_col = ed->cursor_col;
    editor_scroll(ed);

    /* Push to redo stack so the user can redo this */
    undo_push(&buf->redo_stack, rec);
}

void editor_redo(Editor *ed)
{
    Buffer *buf = editor_current_buffer(ed);
    if (!buf) return;

    UndoRecord rec;
    if (!undo_pop(&buf->redo_stack, &rec)) {
        editor_set_status(ed, "Nothing to redo.");
        return;
    }

    /*
     * Re-apply the original operation (forward direction this time).
     * Then restore the cursor to where it was AFTER the original edit,
     * and push the record back onto the undo stack.
     */
    switch (rec.type) {

        case UNDO_INSERT_CHAR:
            /* Redo an insert: put the character back */
            buffer_insert_char(buf, rec.row, rec.col, rec.c);
            break;

        case UNDO_DELETE_CHAR:
            /* Redo a delete: remove the character again */
            buffer_delete_char(buf, rec.row, rec.col);
            break;

        case UNDO_INSERT_NEWLINE:
            /* Redo an Enter: split the line again */
            buffer_insert_newline(buf, rec.row, rec.col);
            break;

        case UNDO_JOIN_LINES:
            /* Redo a join: merge the lines again */
            buffer_join_lines(buf, rec.row);
            break;
    }

    /* Restore cursor to where it was after the original edit */
    ed->cursor_row  = rec.cursor_row_after;
    ed->cursor_col  = rec.cursor_col_after;
    ed->desired_col = ed->cursor_col;
    editor_scroll(ed);

    /* Push back onto the undo stack so the user can undo again */
    undo_push(&buf->undo_stack, rec);
}

/* ============================================================================
 * Misc
 * ============================================================================ */

void editor_set_status(Editor *ed, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(ed->status_msg, STATUS_MSG_LEN, fmt, ap);
    va_end(ap);
}

void editor_scroll(Editor *ed)
{
    Buffer *buf = editor_current_buffer(ed);
    if (!buf) return;

    int rows = editor_rows(ed);
    int cols = editor_cols(ed);

    /* Vertical scrolling */
    if (ed->cursor_row < ed->view_row) {
        ed->view_row = ed->cursor_row;
    }
    if (ed->cursor_row >= ed->view_row + rows) {
        ed->view_row = ed->cursor_row - rows + 1;
    }

    /* Horizontal scrolling */
    if (ed->cursor_col < ed->view_col) {
        ed->view_col = ed->cursor_col;
    }
    if (ed->cursor_col >= ed->view_col + cols) {
        ed->view_col = ed->cursor_col - cols + 1;
    }

    /* Clamp view_row so we don't scroll past the end of the file */
    int max_view_row = buf->num_lines - 1;
    if (ed->view_row > max_view_row) ed->view_row = max_view_row;
    if (ed->view_row < 0)           ed->view_row = 0;
    if (ed->view_col < 0)           ed->view_col = 0;
}
