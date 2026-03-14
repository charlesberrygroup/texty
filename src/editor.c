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

    buffer_insert_char(buf, ed->cursor_row, ed->cursor_col, c);

    ed->cursor_col++;
    ed->desired_col = ed->cursor_col;
    editor_scroll(ed);
}

void editor_insert_newline(Editor *ed)
{
    Buffer *buf = editor_current_buffer(ed);
    if (!buf) return;

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

    if (ed->cursor_col > 0) {
        /* Delete the character to the left of the cursor */
        buffer_delete_char(buf, ed->cursor_row, ed->cursor_col - 1);
        ed->cursor_col--;
    } else if (ed->cursor_row > 0) {
        /* At column 0 — join with the line above */
        int prev_len = buffer_line_len(buf, ed->cursor_row - 1);
        buffer_join_lines(buf, ed->cursor_row - 1);
        ed->cursor_row--;
        ed->cursor_col = prev_len;
    }

    ed->desired_col = ed->cursor_col;
    editor_scroll(ed);
}

void editor_delete_char(Editor *ed)
{
    Buffer *buf = editor_current_buffer(ed);
    if (!buf) return;

    int line_len = buffer_line_len(buf, ed->cursor_row);

    if (ed->cursor_col < line_len) {
        /* Delete the character under the cursor */
        buffer_delete_char(buf, ed->cursor_row, ed->cursor_col);
    } else if (ed->cursor_row < buf->num_lines - 1) {
        /* At end of line — join with the line below */
        buffer_join_lines(buf, ed->cursor_row);
    }
    /* If on the last line at end-of-line, nothing to delete */

    ed->desired_col = ed->cursor_col;
    editor_scroll(ed);
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
