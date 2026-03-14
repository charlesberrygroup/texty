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
 * selection_bounds — compute the normalized start and end of the selection.
 *
 * The anchor and cursor can be in any order.  This function always writes
 * the earlier position to (*sr, *sc) and the later one to (*er, *ec),
 * so the rest of the code does not need to worry about direction.
 */
static void selection_bounds(const Editor *ed,
                              int *sr, int *sc, int *er, int *ec)
{
    int ar = ed->sel_anchor_row, ac = ed->sel_anchor_col;
    int cr = ed->cursor_row,     cc = ed->cursor_col;

    if (ar < cr || (ar == cr && ac <= cc)) {
        *sr = ar; *sc = ac;
        *er = cr; *ec = cc;
    } else {
        *sr = cr; *sc = cc;
        *er = ar; *ec = ac;
    }
}

/*
 * selection_get_text — return a heap-allocated copy of the selected text.
 *
 * The returned string contains '\n' between lines (matching how the buffer
 * stores newlines conceptually).  Caller must free() the result.
 * Returns NULL on allocation failure or if the selection is empty.
 */
static char *selection_get_text(const Editor *ed)
{
    Buffer *buf = editor_current_buffer((Editor *)ed);
    if (!buf || !ed->sel_active) return NULL;

    int sr, sc, er, ec;
    selection_bounds(ed, &sr, &sc, &er, &ec);

    /*
     * Calculate the total number of characters we will copy.
     * For single-line selections: just ec - sc chars.
     * For multi-line: tail of line sr + '\n' + middle lines + '\n' each +
     *                 head of line er.
     */
    int total = 0;
    if (sr == er) {
        total = ec - sc;
    } else {
        total = buffer_line_len(buf, sr) - sc + 1; /* +1 for '\n' */
        for (int r = sr + 1; r < er; r++)
            total += buffer_line_len(buf, r) + 1;  /* +1 for '\n' */
        total += ec;
    }

    if (total <= 0) return NULL;

    /* +1 for the null terminator */
    char *text = malloc(total + 1);
    if (!text) return NULL;

    int pos = 0;

    if (sr == er) {
        /* Single line: copy sc..ec-1 */
        memcpy(text, buffer_get_line(buf, sr) + sc, ec - sc);
        pos = ec - sc;
    } else {
        /* First line: copy sc..end */
        int first_len = buffer_line_len(buf, sr) - sc;
        memcpy(text + pos, buffer_get_line(buf, sr) + sc, first_len);
        pos += first_len;
        text[pos++] = '\n';

        /* Middle lines: copy entire line + '\n' */
        for (int r = sr + 1; r < er; r++) {
            int rlen = buffer_line_len(buf, r);
            memcpy(text + pos, buffer_get_line(buf, r), rlen);
            pos += rlen;
            text[pos++] = '\n';
        }

        /* Last line: copy 0..ec-1 */
        memcpy(text + pos, buffer_get_line(buf, er), ec);
        pos += ec;
    }

    text[pos] = '\0';
    return text;
}

/*
 * insert_text_at — insert a (possibly multi-line) string into the buffer.
 *
 * Iterates over `text`, calling buffer_insert_char for regular characters
 * and buffer_insert_newline for '\n'.  After the call, (*out_row, *out_col)
 * holds the position just past the last inserted character — useful for
 * recording the end of a paste in an UNDO_PASTE record.
 */
static void insert_text_at(Buffer *buf, int row, int col,
                            const char *text,
                            int *out_row, int *out_col)
{
    int cur_row = row, cur_col = col;

    for (int i = 0; text[i]; i++) {
        if (text[i] == '\n') {
            buffer_insert_newline(buf, cur_row, cur_col);
            cur_row++;
            cur_col = 0;
        } else {
            buffer_insert_char(buf, cur_row, cur_col, text[i]);
            cur_col++;
        }
    }

    if (out_row) *out_row = cur_row;
    if (out_col) *out_col = cur_col;
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

    /* Free the internal clipboard if one exists */
    free(ed->clipboard);
    ed->clipboard = NULL;
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
     * If text is selected, delete it first (standard "replace selection"
     * behavior: typing replaces the selected region).
     */
    if (ed->sel_active) {
        int sr, sc, er, ec;
        selection_bounds(ed, &sr, &sc, &er, &ec);
        char *text = selection_get_text(ed);
        if (text) {
            UndoRecord rec;
            rec.type              = UNDO_CUT;
            rec.row               = sr; rec.col = sc;
            rec.end_row           = er; rec.end_col = ec;
            rec.c                 = 0;
            rec.text              = text;
            rec.cursor_row_before = ed->cursor_row;
            rec.cursor_col_before = ed->cursor_col;
            rec.cursor_row_after  = sr;
            rec.cursor_col_after  = sc;
            undo_push(&buf->undo_stack, rec);
            undo_clear(&buf->redo_stack);
            buffer_delete_region(buf, sr, sc, er, ec);
            ed->cursor_row = sr; ed->cursor_col = sc;
        }
        editor_selection_clear(ed);
    }

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

    /* Delete the selection if one is active (Backspace clears it) */
    if (ed->sel_active) {
        editor_cut(ed);
        return;
    }

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

    /* Delete the selection if one is active (Delete key clears it) */
    if (ed->sel_active) {
        editor_cut(ed);
        return;
    }

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
 * Selection
 * ============================================================================ */

void editor_selection_clear(Editor *ed)
{
    ed->sel_active = 0;
}

/*
 * selection_ensure_anchor — if no selection is active, start one at the
 * current cursor position.  Called at the start of every Shift+Arrow handler.
 */
static void selection_ensure_anchor(Editor *ed)
{
    if (!ed->sel_active) {
        ed->sel_anchor_row = ed->cursor_row;
        ed->sel_anchor_col = ed->cursor_col;
        ed->sel_active     = 1;
    }
}

void editor_select_left(Editor *ed)
{
    selection_ensure_anchor(ed);
    editor_move_left(ed);
}

void editor_select_right(Editor *ed)
{
    selection_ensure_anchor(ed);
    editor_move_right(ed);
}

void editor_select_up(Editor *ed)
{
    selection_ensure_anchor(ed);
    editor_move_up(ed);
}

void editor_select_down(Editor *ed)
{
    selection_ensure_anchor(ed);
    editor_move_down(ed);
}

void editor_select_line_start(Editor *ed)
{
    selection_ensure_anchor(ed);
    editor_move_line_start(ed);
}

void editor_select_line_end(Editor *ed)
{
    selection_ensure_anchor(ed);
    editor_move_line_end(ed);
}

void editor_select_all(Editor *ed)
{
    Buffer *buf = editor_current_buffer(ed);
    if (!buf) return;

    /* Anchor at the very beginning of the file */
    ed->sel_anchor_row = 0;
    ed->sel_anchor_col = 0;
    ed->sel_active     = 1;

    /* Cursor at the very end of the file */
    ed->cursor_row  = buf->num_lines - 1;
    ed->cursor_col  = buffer_line_len(buf, ed->cursor_row);
    ed->desired_col = ed->cursor_col;

    editor_scroll(ed);
}

/* ============================================================================
 * Clipboard: copy, cut, paste
 * ============================================================================ */

void editor_copy(Editor *ed)
{
    if (!ed->sel_active) {
        editor_set_status(ed, "No selection to copy.  Use Shift+Arrow to select.");
        return;
    }

    char *text = selection_get_text(ed);
    if (!text) {
        editor_set_status(ed, "Copy failed (empty selection or out of memory).");
        return;
    }

    /* Replace the existing clipboard contents */
    free(ed->clipboard);
    ed->clipboard = text;

    editor_set_status(ed, "Copied.");
    /* Leave the selection active so the user can see what was copied */
}

void editor_cut(Editor *ed)
{
    Buffer *buf = editor_current_buffer(ed);
    if (!buf) return;

    if (!ed->sel_active) {
        editor_set_status(ed, "No selection to cut.  Use Shift+Arrow to select.");
        return;
    }

    int sr, sc, er, ec;
    selection_bounds(ed, &sr, &sc, &er, &ec);

    /* Grab the text before deleting it */
    char *text = selection_get_text(ed);
    if (!text) {
        editor_set_status(ed, "Cut failed (out of memory).");
        return;
    }

    /* Update the clipboard */
    free(ed->clipboard);
    ed->clipboard = text;

    /*
     * Record an UNDO_CUT entry.  We store a duplicate of the text in the
     * undo record so that if the user pastes after cutting (overwriting the
     * clipboard), undo can still restore the original cut text.
     *
     * strdup() allocates a new copy of the string — the undo stack owns it.
     */
    UndoRecord rec;
    rec.type              = UNDO_CUT;
    rec.row               = sr;
    rec.col               = sc;
    rec.end_row           = er;
    rec.end_col           = ec;
    rec.c                 = 0;
    rec.text              = strdup(text);   /* undo stack takes ownership */
    rec.cursor_row_before = ed->cursor_row;
    rec.cursor_col_before = ed->cursor_col;
    rec.cursor_row_after  = sr;
    rec.cursor_col_after  = sc;

    undo_push(&buf->undo_stack, rec);
    undo_clear(&buf->redo_stack);

    /* Delete the selected region */
    buffer_delete_region(buf, sr, sc, er, ec);

    /* Move cursor to where the selection started */
    ed->cursor_row  = sr;
    ed->cursor_col  = sc;
    ed->desired_col = sc;
    editor_selection_clear(ed);
    editor_scroll(ed);

    editor_set_status(ed, "Cut.");
}

void editor_paste(Editor *ed)
{
    Buffer *buf = editor_current_buffer(ed);
    if (!buf) return;

    if (!ed->clipboard) {
        editor_set_status(ed, "Clipboard is empty.");
        return;
    }

    /*
     * If a selection is active, delete it first (standard "replace selection"
     * behavior — typing or pasting over a selection removes it).
     */
    if (ed->sel_active) {
        editor_cut(ed);  /* this updates clipboard with selected text though */
        /*
         * That just replaced our clipboard.  We need to paste the original
         * clipboard.  Restore it from the cut record: the old clipboard is
         * now in buf->undo_stack.  For simplicity, we paste whatever is in
         * ed->clipboard now (which is the cut text).  This is a known
         * limitation — replacing a selection then pasting uses the selection
         * text, not the previous clipboard.  A proper implementation would
         * save/restore the clipboard around the cut.  We'll revisit in a
         * later phase.
         */
    }

    if (!ed->clipboard) return;

    int paste_row = ed->cursor_row;
    int paste_col = ed->cursor_col;

    /* Insert the clipboard text, tracking where it ends */
    int end_row, end_col;
    insert_text_at(buf, paste_row, paste_col, ed->clipboard, &end_row, &end_col);

    /*
     * Record UNDO_PASTE.  We store a copy of the pasted text so undo can
     * re-paste it (for redo) without relying on the clipboard still having
     * the same content.
     */
    UndoRecord rec;
    rec.type              = UNDO_PASTE;
    rec.row               = paste_row;
    rec.col               = paste_col;
    rec.end_row           = end_row;
    rec.end_col           = end_col;
    rec.c                 = 0;
    rec.text              = strdup(ed->clipboard);
    rec.cursor_row_before = paste_row;
    rec.cursor_col_before = paste_col;
    rec.cursor_row_after  = end_row;
    rec.cursor_col_after  = end_col;

    undo_push(&buf->undo_stack, rec);
    undo_clear(&buf->redo_stack);

    /* Move cursor to end of pasted text */
    ed->cursor_row  = end_row;
    ed->cursor_col  = end_col;
    ed->desired_col = end_col;
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

        case UNDO_CUT:
            /*
             * Undo a cut: re-insert the text that was deleted.
             * rec.text is the cut text; rec.row/col is where it was cut from.
             */
            insert_text_at(buf, rec.row, rec.col, rec.text, NULL, NULL);
            break;

        case UNDO_PASTE:
            /*
             * Undo a paste: delete the text that was inserted.
             * The pasted text occupies (rec.row, rec.col) to (rec.end_row, rec.end_col).
             */
            buffer_delete_region(buf, rec.row, rec.col, rec.end_row, rec.end_col);
            break;
    }

    /* Restore cursor to where it was before the original edit */
    ed->cursor_row  = rec.cursor_row_before;
    ed->cursor_col  = rec.cursor_col_before;
    ed->desired_col = ed->cursor_col;
    editor_selection_clear(ed);
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

        case UNDO_CUT:
            /* Redo a cut: delete the region again */
            buffer_delete_region(buf, rec.row, rec.col, rec.end_row, rec.end_col);
            break;

        case UNDO_PASTE:
            /* Redo a paste: re-insert the text */
            insert_text_at(buf, rec.row, rec.col, rec.text, NULL, NULL);
            break;
    }

    /* Restore cursor to where it was after the original edit */
    ed->cursor_row  = rec.cursor_row_after;
    ed->cursor_col  = rec.cursor_col_after;
    ed->desired_col = ed->cursor_col;
    editor_selection_clear(ed);
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
