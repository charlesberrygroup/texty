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
#include "display.h"    /* for GUTTER_WIDTH, FILETREE_WIDTH */
#include "undo.h"
#include "filetree.h"   /* for FileTree, filetree_create, filetree_rebuild, etc. */
#include "git.h"        /* for git_refresh — called on open/save */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

/* ============================================================================
 * Internal helpers
 * ============================================================================ */

/* Forward declarations — defined further down, but called from editor_save and
 * buffer-switching functions which are earlier in the file. */
static void refresh_inline_diff(Editor *ed);
static void refresh_git_blame(Editor *ed);

/*
 * get_repo_root — get the git repo root for the current buffer, with CWD fallback.
 *
 * Tries the buffer's git_state.repo_root first.  If that's NULL (e.g. the
 * editor was opened without a file), falls back to detecting the repo from
 * the current working directory.
 *
 * Returns a heap-allocated string that the caller must free(), or NULL if
 * not in a git repo.
 */
static char *get_repo_root(Editor *ed)
{
    Buffer *buf = editor_current_buffer(ed);
    if (buf && buf->git_state.repo_root && buf->git_state.repo_root[0] != '\0')
        return strdup(buf->git_state.repo_root);

    return git_find_repo_root(NULL);
}

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
 * We reserve 1 row at the bottom for the status bar and TAB_BAR_HEIGHT rows
 * at the top for the tab bar.
 */
static int editor_rows(const Editor *ed)
{
    return ed->term_rows - 1 - TAB_BAR_HEIGHT;
}

/*
 * editor_cols — how many columns are available for text content.
 *
 * We subtract the gutter width (line numbers) from the total terminal width.
 * If the file tree panel is visible, we also subtract its width so the editor
 * area is correctly narrowed — preventing text from being drawn under the panel.
 */
static int editor_cols(const Editor *ed)
{
    /*
     * panel_w is the width consumed by the file explorer panel.
     * It is non-zero only when the panel is both shown AND initialised
     * (ed->filetree != NULL).  We guard on both conditions to avoid
     * using FILETREE_WIDTH before the panel has been created.
     */
    int panel_w     = (ed->show_filetree && ed->filetree) ? FILETREE_WIDTH : 0;
    int git_panel_w = (ed->show_git_panel && ed->git_status) ? GIT_PANEL_WIDTH : 0;
    int blame_w     = ed->show_git_blame ? BLAME_WIDTH : 0;
    return ed->term_cols - GUTTER_WIDTH - panel_w - git_panel_w - blame_w;
}

/* ============================================================================
 * Lifecycle
 * ============================================================================ */

void editor_init(Editor *ed)
{
    memset(ed, 0, sizeof(Editor));
    /*
     * -1 means "no match found yet".  memset sets them to 0, so we
     * override here to avoid treating row 0 as a valid match on startup.
     */
    ed->search_match_row = -1;
    ed->search_match_col = -1;
    ed->tab_width        = 4;   /* default: 4 spaces per Tab */
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

    /*
     * Free the file explorer tree if it was ever created.
     * filetree_free(NULL) is defined as a no-op in filetree.c, so this is
     * safe even if the user never opened the file panel during this session.
     */
    if (ed->filetree) {
        filetree_free(ed->filetree);
        ed->filetree = NULL;
    }

    /* Free the git status list if it was ever created */
    if (ed->git_status) {
        git_status_free(ed->git_status);
        free(ed->git_status);
        ed->git_status = NULL;
    }

    /* Free inline diff chunks */
    git_diff_chunks_free(&ed->inline_diff);

    /* Free blame data */
    git_blame_free(&ed->git_blame);
}

/* ============================================================================
 * Buffer management — internal cursor save/restore helpers
 * ============================================================================ */

/*
 * save_cursor_to_buffer — copy the editor's live cursor/viewport state into
 * the current buffer's saved-state fields.
 *
 * Call this BEFORE switching away from a buffer so the position is preserved.
 */
static void save_cursor_to_buffer(Editor *ed)
{
    if (ed->num_buffers == 0) return;
    Buffer *buf      = ed->buffers[ed->current_buffer];
    buf->cursor_row  = ed->cursor_row;
    buf->cursor_col  = ed->cursor_col;
    buf->desired_col = ed->desired_col;
    buf->view_row    = ed->view_row;
    buf->view_col    = ed->view_col;
}

/*
 * restore_cursor_from_buffer — load the current buffer's saved cursor/viewport
 * state back into the editor's live fields.
 *
 * Call this AFTER switching to a new buffer so the cursor appears where it was
 * last time the user had that buffer open.
 */
static void restore_cursor_from_buffer(Editor *ed)
{
    if (ed->num_buffers == 0) return;
    Buffer *buf      = ed->buffers[ed->current_buffer];
    ed->cursor_row   = buf->cursor_row;
    ed->cursor_col   = buf->cursor_col;
    ed->desired_col  = buf->desired_col;
    ed->view_row     = buf->view_row;
    ed->view_col     = buf->view_col;
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

    /* Save cursor state for the buffer we are leaving (if any) */
    if (ed->num_buffers > 0)
        save_cursor_to_buffer(ed);

    Buffer *buf = buffer_create();
    if (!buf) {
        editor_set_status(ed, "Error: out of memory");
        return -1;
    }

    int idx = ed->num_buffers;
    ed->buffers[idx] = buf;
    ed->num_buffers++;
    ed->current_buffer = idx;

    /* New buffer always starts at the top-left — restore from the fresh buffer */
    restore_cursor_from_buffer(ed);
    editor_selection_clear(ed);

    return 0;
}

int editor_open_file(Editor *ed, const char *path)
{
    if (ed->num_buffers >= EDITOR_MAX_BUFFERS) {
        editor_set_status(ed, "Error: too many open buffers");
        return -1;
    }

    /* Save cursor state for the buffer we are leaving (if any) */
    if (ed->num_buffers > 0)
        save_cursor_to_buffer(ed);

    Buffer *buf = buffer_create();
    if (!buf) {
        editor_set_status(ed, "Error: out of memory");
        return -1;
    }

    if (buffer_load_file(buf, path) != 0) {
        /* File might not exist yet — treat as a new file with the given name */
        free(buf->filename);
        buf->filename = strdup(path);
    }

    int idx = ed->num_buffers;
    ed->buffers[idx] = buf;
    ed->num_buffers++;
    ed->current_buffer = idx;

    /* New buffer starts at the top-left */
    restore_cursor_from_buffer(ed);
    editor_selection_clear(ed);

    /* Detect git repo and populate line change markers */
    if (buf->filename)
        git_refresh(&buf->git_state, buf->filename, buf->num_lines);

    return 0;
}

int editor_open_or_switch(Editor *ed, const char *path)
{
    /*
     * Scan all open buffers for one whose filename matches `path`.
     * strcmp() returns 0 when the strings are equal.
     * We check buf->filename for NULL first (unsaved buffers have no name).
     */
    for (int i = 0; i < ed->num_buffers; i++) {
        Buffer *buf = ed->buffers[i];
        if (buf && buf->filename && strcmp(buf->filename, path) == 0) {
            /* File is already open — switch to it */
            save_cursor_to_buffer(ed);
            ed->current_buffer = i;
            restore_cursor_from_buffer(ed);
            editor_selection_clear(ed);
            editor_set_status(ed, "Switched to existing buffer");
            return 0;
        }
    }

    /* Not already open — open it in a new buffer */
    return editor_open_file(ed, path);
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

    /* Refresh git line markers after saving */
    git_refresh(&buf->git_state, buf->filename, buf->num_lines);

    /* Refresh inline diff chunks if the view is active */
    if (ed->show_inline_diff)
        refresh_inline_diff(ed);

    return 0;
}

Buffer *editor_current_buffer(Editor *ed)
{
    if (ed->num_buffers == 0) return NULL;
    return ed->buffers[ed->current_buffer];
}

void editor_next_buffer(Editor *ed)
{
    if (ed->num_buffers <= 1) return;   /* nothing to switch to */

    save_cursor_to_buffer(ed);
    editor_selection_clear(ed);

    /* Advance to the next buffer, wrapping from last back to first */
    ed->current_buffer = (ed->current_buffer + 1) % ed->num_buffers;

    restore_cursor_from_buffer(ed);

    /* Refresh inline diff for the new buffer if the view is active */
    if (ed->show_inline_diff)
        refresh_inline_diff(ed);
    if (ed->show_git_blame)
        refresh_git_blame(ed);
}

void editor_prev_buffer(Editor *ed)
{
    if (ed->num_buffers <= 1) return;

    save_cursor_to_buffer(ed);
    editor_selection_clear(ed);

    /* Move to the previous buffer, wrapping from first to last */
    ed->current_buffer = (ed->current_buffer - 1 + ed->num_buffers)
                         % ed->num_buffers;

    restore_cursor_from_buffer(ed);

    /* Refresh inline diff for the new buffer if the view is active */
    if (ed->show_inline_diff)
        refresh_inline_diff(ed);
    if (ed->show_git_blame)
        refresh_git_blame(ed);
}

void editor_close_buffer(Editor *ed)
{
    Buffer *buf = editor_current_buffer(ed);
    if (!buf) return;

    /*
     * If the buffer has unsaved changes, require a second Ctrl+W to confirm.
     * We use a static flag (same pattern as the quit confirmation).
     */
    if (buf->dirty) {
        static int warn_pending = 0;
        if (!warn_pending) {
            editor_set_status(ed,
                "Unsaved changes! Press Ctrl+W again to close without saving.");
            warn_pending = 1;
            return;
        }
        warn_pending = 0;
    }

    int idx = ed->current_buffer;

    /* Free the buffer and its undo/redo text pointers */
    buffer_destroy(ed->buffers[idx]);

    /*
     * Shift all buffers after `idx` one slot to the left to close the gap.
     * memmove is safe for overlapping regions.
     */
    memmove(&ed->buffers[idx],
            &ed->buffers[idx + 1],
            (ed->num_buffers - idx - 1) * sizeof(Buffer *));

    ed->num_buffers--;

    /*
     * If we just closed the last buffer, create a new empty one so the
     * editor always has something to display.
     */
    if (ed->num_buffers == 0) {
        editor_new_buffer(ed);
        return;
    }

    /* Clamp current_buffer so it points to a valid buffer */
    if (ed->current_buffer >= ed->num_buffers)
        ed->current_buffer = ed->num_buffers - 1;

    restore_cursor_from_buffer(ed);
    editor_selection_clear(ed);
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
            memset(&rec, 0, sizeof(rec));
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
    memset(&rec, 0, sizeof(rec));
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

void editor_insert_pair(Editor *ed, char open, char close)
{
    Buffer *buf = editor_current_buffer(ed);
    if (!buf) return;

    /*
     * If text is selected, delete the selection first (same behaviour as
     * editor_insert_char: typing replaces the selection).
     */
    if (ed->sel_active) {
        int sr, sc, er, ec;
        selection_bounds(ed, &sr, &sc, &er, &ec);
        char *text = selection_get_text(ed);
        if (text) {
            UndoRecord rec;
            memset(&rec, 0, sizeof(rec));
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
     * Build the two-character string: open char + close char.
     * insert_text_at handles inserting the whole thing at once.
     */
    char pair[3] = { open, close, '\0' };
    int start_row = ed->cursor_row;
    int start_col = ed->cursor_col;
    int end_row, end_col;

    insert_text_at(buf, start_row, start_col, pair, &end_row, &end_col);

    /*
     * Record as a single UNDO_PASTE entry so both chars are removed with
     * one Ctrl+Z.  end_row/end_col mark where the inserted text ends
     * (needed so undo knows how much to delete).
     *
     * cursor_col_after is start_col+1 — BETWEEN the two chars — not the
     * end of the pair.  This way redo also restores the cursor between them.
     */
    UndoRecord rec;
    memset(&rec, 0, sizeof(rec));
    rec.type              = UNDO_PASTE;
    rec.row               = start_row;
    rec.col               = start_col;
    rec.end_row           = end_row;
    rec.end_col           = end_col;
    rec.text              = strdup(pair);   /* freed by undo_clear/undo_push */
    rec.cursor_row_before = start_row;
    rec.cursor_col_before = start_col;
    rec.cursor_row_after  = start_row;      /* cursor lands between the pair */
    rec.cursor_col_after  = start_col + 1;
    undo_push(&buf->undo_stack, rec);
    undo_clear(&buf->redo_stack);

    /* Place cursor between the opening and closing character */
    ed->cursor_row  = start_row;
    ed->cursor_col  = start_col + 1;
    ed->desired_col = ed->cursor_col;
    editor_scroll(ed);
}

int editor_find_bracket_match(const Editor *ed, int *out_row, int *out_col)
{
    const Buffer *buf = ed->buffers[ed->current_buffer];
    if (!buf) return 0;

    int row = ed->cursor_row;
    int col = ed->cursor_col;
    int len = buffer_line_len(buf, row);

    /* No character under the cursor (cursor is past end of line) */
    if (col >= len) return 0;

    char c = buffer_get_line(buf, row)[col];

    /*
     * Determine search direction and the open/close pair to match.
     * forward = 1: cursor is on the opening bracket, scan forward.
     * forward = 0: cursor is on the closing bracket, scan backward.
     */
    char open, close;
    int  forward;

    if      (c == '(') { open = '('; close = ')'; forward = 1; }
    else if (c == '[') { open = '['; close = ']'; forward = 1; }
    else if (c == '{') { open = '{'; close = '}'; forward = 1; }
    else if (c == ')') { open = '('; close = ')'; forward = 0; }
    else if (c == ']') { open = '['; close = ']'; forward = 0; }
    else if (c == '}') { open = '{'; close = '}'; forward = 0; }
    else return 0;   /* not a bracket character */

    /*
     * depth starts at 0.  Each time we see the same bracket type as the
     * starting one, depth increases.  Each time we see the opposite, depth
     * decreases.  When depth reaches -1, we have found the match.
     *
     * We start depth at 0 and begin scanning at the cursor position (which
     * will immediately push depth to 1 for forward, or -1+1=0 for backward…
     * actually we skip the starting char by adjusting the loop start).
     */
    int depth = 0;

    if (forward) {
        /*
         * Scan forward through every line and column from (row, col).
         * We include the starting column so the opening bracket itself sets
         * depth to 1, and we stop when depth drops back to 0.
         */
        for (int r = row; r < buf->num_lines; r++) {
            const char *line = buffer_get_line(buf, r);
            int llen         = buffer_line_len(buf, r);
            int start        = (r == row) ? col : 0;

            for (int k = start; k < llen; k++) {
                if      (line[k] == open)  depth++;
                else if (line[k] == close) depth--;

                /* depth == 0 means we matched the opening bracket at (row,col) */
                if (depth == 0) {
                    *out_row = r;
                    *out_col = k;
                    return 1;
                }
            }
        }
    } else {
        /*
         * Scan backward from (row, col) toward the start of the file.
         * The closing bracket at (row, col) sets depth to -1; we stop when
         * depth returns to 0.
         */
        for (int r = row; r >= 0; r--) {
            const char *line = buffer_get_line(buf, r);
            int llen         = buffer_line_len(buf, r);
            int start        = (r == row) ? col : llen - 1;

            for (int k = start; k >= 0; k--) {
                if      (line[k] == close) depth--;
                else if (line[k] == open)  depth++;

                /* depth == 0 means we matched the closing bracket at (row,col) */
                if (depth == 0) {
                    *out_row = r;
                    *out_col = k;
                    return 1;
                }
            }
        }
    }

    return 0;   /* no matching bracket found */
}

void editor_goto_line(Editor *ed)
{
    Buffer *buf = editor_current_buffer(ed);
    if (!buf) return;

    /*
     * Prompt the user for a line number.  display_prompt() draws an input
     * field in the status bar and returns the typed string, or NULL if the
     * user pressed Escape.
     */
    char *input = display_prompt(ed, "Go to line: ");
    if (!input) return;   /* user cancelled */

    /*
     * Parse the input as an integer.  atoi() returns 0 for non-numeric
     * input, which we treat as "no valid number entered".
     */
    int target = atoi(input);
    free(input);

    if (target < 1) {
        editor_set_status(ed, "Invalid line number.");
        return;
    }

    /*
     * Line numbers shown to the user are 1-based; internally they are 0-based.
     * Clamp to the valid range so we never set cursor_row out of bounds.
     */
    int row = target - 1;
    if (row >= buf->num_lines)
        row = buf->num_lines - 1;

    ed->cursor_row  = row;
    ed->cursor_col  = 0;
    ed->desired_col = 0;
    editor_selection_clear(ed);
    editor_scroll(ed);
    editor_set_status(ed, "Line %d", row + 1);
}

void editor_toggle_word_wrap(Editor *ed)
{
    ed->word_wrap = !ed->word_wrap;
    /*
     * Reset horizontal scroll when turning wrap on — there is nothing to
     * scroll horizontally when lines are wrapped.
     */
    if (ed->word_wrap)
        ed->view_col = 0;
    editor_set_status(ed, "Word wrap %s (F4 to toggle)",
                      ed->word_wrap ? "ON" : "OFF");
    editor_scroll(ed);
}

void editor_toggle_whitespace(Editor *ed)
{
    ed->show_whitespace = !ed->show_whitespace;
    editor_set_status(ed, "Whitespace display %s (F2 to toggle)",
                      ed->show_whitespace ? "ON" : "OFF");
}

void editor_insert_tab(Editor *ed)
{
    Buffer *buf = editor_current_buffer(ed);
    if (!buf) return;

    /*
     * Build a string of tab_width spaces.  We cap at 8 to guard against a
     * misconfigured tab_width, though 4 is the default.
     */
    int  w = (ed->tab_width > 0 && ed->tab_width <= 8) ? ed->tab_width : 4;
    char spaces[9];              /* max 8 spaces + null terminator */
    for (int i = 0; i < w; i++) spaces[i] = ' ';
    spaces[w] = '\0';

    /*
     * Insert the spaces using the same helper as paste.  This lets us record
     * the whole tab as a SINGLE undo entry — one Ctrl+Z removes all spaces.
     */
    int start_row = ed->cursor_row;
    int start_col = ed->cursor_col;
    int end_row, end_col;

    insert_text_at(buf, start_row, start_col, spaces, &end_row, &end_col);

    /* Record as UNDO_PASTE with a copy of the spaces string */
    UndoRecord rec;
    memset(&rec, 0, sizeof(rec));
    rec.type              = UNDO_PASTE;
    rec.row               = start_row;
    rec.col               = start_col;
    rec.end_row           = end_row;
    rec.end_col           = end_col;
    rec.text              = strdup(spaces);  /* freed by undo_clear/undo_push */
    rec.cursor_row_before = start_row;
    rec.cursor_col_before = start_col;
    rec.cursor_row_after  = end_row;
    rec.cursor_col_after  = end_col;
    undo_push(&buf->undo_stack, rec);
    undo_clear(&buf->redo_stack);

    /* Move cursor to end of inserted spaces */
    ed->cursor_row  = end_row;
    ed->cursor_col  = end_col;
    ed->desired_col = ed->cursor_col;
    editor_scroll(ed);
}

void editor_insert_newline(Editor *ed)
{
    Buffer *buf = editor_current_buffer(ed);
    if (!buf) return;

    /*
     * Auto-indent: capture the leading whitespace of the current line BEFORE
     * we split it, so we can copy it onto the new line afterward.
     *
     * We copy into a fixed buffer rather than keeping a pointer into
     * buf->lines, because buffer_insert_newline may realloc the lines array,
     * which would invalidate any pointer we held into it.
     */
    Line *cur_line = &buf->lines[ed->cursor_row];
    int indent = 0;
    char indent_buf[256];   /* enough for any reasonable indentation */
    while (indent < cur_line->len &&
           indent < (int)(sizeof(indent_buf)) &&
           (cur_line->text[indent] == ' ' || cur_line->text[indent] == '\t')) {
        indent_buf[indent] = cur_line->text[indent];
        indent++;
    }

    /*
     * Record the split point.  To undo, we call join_lines(row), which
     * joins line `row` with line `row+1` — exactly what Enter split apart.
     * The cursor_col_after accounts for the indentation that will be inserted.
     */
    UndoRecord rec;
    memset(&rec, 0, sizeof(rec));
    rec.type              = UNDO_INSERT_NEWLINE;
    rec.row               = ed->cursor_row;
    rec.col               = ed->cursor_col;
    rec.c                 = 0;               /* not used for newline */
    rec.cursor_row_before = ed->cursor_row;
    rec.cursor_col_before = ed->cursor_col;
    rec.cursor_row_after  = ed->cursor_row + 1;
    rec.cursor_col_after  = indent;          /* cursor lands after the indent */
    undo_push(&buf->undo_stack, rec);

    undo_clear(&buf->redo_stack);

    buffer_insert_newline(buf, ed->cursor_row, ed->cursor_col);

    ed->cursor_row++;
    ed->cursor_col  = 0;
    ed->desired_col = 0;

    /*
     * Insert the indentation characters into the new (empty) line.
     * We insert them one at a time using buffer_insert_char, which keeps
     * the buffer's internal state consistent.
     *
     * Note: we do NOT push individual undo records for each indent character —
     * the single UNDO_INSERT_NEWLINE record above records the final cursor
     * position (col = indent), so undoing the newline removes the whole line
     * including its indentation prefix.
     */
    for (int i = 0; i < indent; i++) {
        buffer_insert_char(buf, ed->cursor_row, ed->cursor_col,
                           indent_buf[i]);
        ed->cursor_col++;
    }

    ed->desired_col = ed->cursor_col;
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
    memset(&rec, 0, sizeof(rec));

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
    memset(&rec, 0, sizeof(rec));

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
 * Search & Replace — internal helpers
 * ============================================================================ */

/*
 * search_forward — find the first occurrence of `query` (length `qlen`) at or
 * after position (from_row, from_col) in the buffer.
 *
 * Uses strncmp for a straightforward byte-by-byte match (case-sensitive).
 * Does NOT wrap — callers handle wrapping.
 *
 * Returns 1 and sets *out_row / *out_col if a match is found, 0 otherwise.
 */
static int search_forward(Buffer *buf, const char *query, int qlen,
                           int from_row, int from_col,
                           int *out_row, int *out_col)
{
    for (int row = from_row; row < buf->num_lines; row++) {
        const char *line     = buffer_get_line(buf, row);
        int         line_len = buffer_line_len(buf, row);
        int         col_start = (row == from_row) ? from_col : 0;

        for (int col = col_start; col + qlen <= line_len; col++) {
            if (strncmp(line + col, query, qlen) == 0) {
                *out_row = row;
                *out_col = col;
                return 1;
            }
        }
    }
    return 0;
}

/*
 * search_backward — find the last occurrence of `query` strictly before
 * position (from_row, from_col).
 *
 * Scans lines in reverse, and within each line scans columns in reverse
 * to find the rightmost match that precedes the given position.
 *
 * Returns 1 and sets *out_row / *out_col if found, 0 otherwise.
 */
static int search_backward(Buffer *buf, const char *query, int qlen,
                            int from_row, int from_col,
                            int *out_row, int *out_col)
{
    for (int row = from_row; row >= 0; row--) {
        const char *line     = buffer_get_line(buf, row);
        int         line_len = buffer_line_len(buf, row);

        /*
         * On the starting row, only consider columns strictly before from_col.
         * The -1 means the match must START before from_col.
         */
        int col_end = (row == from_row) ? from_col - 1 : line_len - qlen;

        for (int col = col_end; col >= 0; col--) {
            if (col + qlen <= line_len
                    && strncmp(line + col, query, qlen) == 0) {
                *out_row = row;
                *out_col = col;
                return 1;
            }
        }
    }
    return 0;
}

/*
 * jump_to_match — move the editor cursor to the found match and scroll.
 */
static void jump_to_match(Editor *ed, int row, int col)
{
    ed->search_match_row = row;
    ed->search_match_col = col;
    ed->cursor_row       = row;
    ed->cursor_col       = col;
    ed->desired_col      = col;
    editor_scroll(ed);
}

/* ============================================================================
 * Search & Replace — public API
 * ============================================================================ */

void editor_search_clear(Editor *ed)
{
    ed->search_query[0]  = '\0';
    ed->search_match_row = -1;
    ed->search_match_col = -1;
}

void editor_find(Editor *ed)
{
    Buffer *buf = editor_current_buffer(ed);
    if (!buf) return;

    char *query = display_prompt(ed, "Find: ");
    if (!query) return;   /* user pressed Escape */

    if (query[0] == '\0') {
        /* Empty query clears the search */
        editor_search_clear(ed);
        free(query);
        return;
    }

    /* Store the query */
    strncpy(ed->search_query, query, sizeof(ed->search_query) - 1);
    ed->search_query[sizeof(ed->search_query) - 1] = '\0';
    free(query);

    int qlen = (int)strlen(ed->search_query);
    int match_row, match_col;

    /*
     * Start searching from the cursor position.  If we find a match
     * at the cursor itself, that counts — useful when re-searching after
     * editing.  For "find next" we start one column ahead.
     */
    if (search_forward(buf, ed->search_query, qlen,
                        ed->cursor_row, ed->cursor_col,
                        &match_row, &match_col)) {
        jump_to_match(ed, match_row, match_col);
        editor_set_status(ed, "F3: next  Shift+F3: prev  Ctrl+R: replace  Esc: clear");
    } else if (search_forward(buf, ed->search_query, qlen,
                                0, 0, &match_row, &match_col)) {
        /* Wrap around from the top */
        jump_to_match(ed, match_row, match_col);
        editor_set_status(ed, "Wrapped.  F3: next  Shift+F3: prev  Ctrl+R: replace");
    } else {
        ed->search_match_row = -1;
        ed->search_match_col = -1;
        editor_set_status(ed, "Not found: %s", ed->search_query);
    }
}

void editor_find_next(Editor *ed)
{
    Buffer *buf = editor_current_buffer(ed);
    if (!buf) return;

    /* If no query is set, open the find prompt */
    if (ed->search_query[0] == '\0') {
        editor_find(ed);
        return;
    }

    int qlen      = (int)strlen(ed->search_query);
    int from_row  = (ed->search_match_row >= 0) ? ed->search_match_row : ed->cursor_row;
    int from_col  = (ed->search_match_col >= 0) ? ed->search_match_col + qlen
                                                 : ed->cursor_col + 1;
    int match_row, match_col;

    if (search_forward(buf, ed->search_query, qlen,
                        from_row, from_col, &match_row, &match_col)) {
        jump_to_match(ed, match_row, match_col);
        editor_set_status(ed, "F3: next  Shift+F3: prev  Ctrl+R: replace  Esc: clear");
    } else if (search_forward(buf, ed->search_query, qlen,
                                0, 0, &match_row, &match_col)) {
        jump_to_match(ed, match_row, match_col);
        editor_set_status(ed, "Wrapped to first match.");
    } else {
        editor_set_status(ed, "No matches for: %s", ed->search_query);
    }
}

void editor_find_prev(Editor *ed)
{
    Buffer *buf = editor_current_buffer(ed);
    if (!buf) return;

    if (ed->search_query[0] == '\0') {
        editor_find(ed);
        return;
    }

    int qlen     = (int)strlen(ed->search_query);
    int from_row = (ed->search_match_row >= 0) ? ed->search_match_row : ed->cursor_row;
    int from_col = (ed->search_match_col >= 0) ? ed->search_match_col
                                                : ed->cursor_col;
    int match_row, match_col;

    if (search_backward(buf, ed->search_query, qlen,
                         from_row, from_col, &match_row, &match_col)) {
        jump_to_match(ed, match_row, match_col);
        editor_set_status(ed, "F3: next  Shift+F3: prev  Ctrl+R: replace  Esc: clear");
    } else {
        /* Wrap: find the last match in the buffer */
        int last_row = buf->num_lines - 1;
        int last_col = buffer_line_len(buf, last_row);
        if (search_backward(buf, ed->search_query, qlen,
                             last_row, last_col + 1, &match_row, &match_col)) {
            jump_to_match(ed, match_row, match_col);
            editor_set_status(ed, "Wrapped to last match.");
        } else {
            editor_set_status(ed, "No matches for: %s", ed->search_query);
        }
    }
}

void editor_replace(Editor *ed)
{
    Buffer *buf = editor_current_buffer(ed);
    if (!buf) return;

    /* Prompt for the search string */
    char *query = display_prompt(ed, "Find: ");
    if (!query || query[0] == '\0') {
        free(query);
        return;
    }

    /* Prompt for the replacement */
    char *replacement = display_prompt(ed, "Replace with: ");
    if (!replacement) {
        free(query);
        return;
    }

    /* Store the query so highlights update */
    strncpy(ed->search_query, query, sizeof(ed->search_query) - 1);
    ed->search_query[sizeof(ed->search_query) - 1] = '\0';

    int qlen  = (int)strlen(query);
    int count = 0;

    /*
     * Scan forward through the buffer replacing every occurrence.
     * After each replacement, continue scanning from the end of the
     * inserted replacement text to avoid re-matching within it.
     *
     * Each replacement is recorded as an UNDO_CUT + UNDO_PASTE pair so the
     * user can step backwards through them with Ctrl+Z.
     */
    int row = 0, col = 0;
    int match_row, match_col;

    while (search_forward(buf, query, qlen, row, col, &match_row, &match_col)) {

        /* --- Record UNDO_CUT for the deleted match --- */
        UndoRecord cut_rec;
        memset(&cut_rec, 0, sizeof(cut_rec));
        cut_rec.type              = UNDO_CUT;
        cut_rec.row               = match_row;
        cut_rec.col               = match_col;
        cut_rec.end_row           = match_row;
        cut_rec.end_col           = match_col + qlen;
        cut_rec.text              = strdup(query);   /* undo stack owns this */
        cut_rec.cursor_row_before = match_row;
        cut_rec.cursor_col_before = match_col;
        cut_rec.cursor_row_after  = match_row;
        cut_rec.cursor_col_after  = match_col;
        undo_push(&buf->undo_stack, cut_rec);

        /* Delete the matched text */
        buffer_delete_region(buf, match_row, match_col,
                                   match_row, match_col + qlen);

        /* Insert the replacement */
        int end_row, end_col;
        insert_text_at(buf, match_row, match_col, replacement,
                        &end_row, &end_col);

        /* --- Record UNDO_PASTE for the inserted replacement --- */
        UndoRecord paste_rec;
        memset(&paste_rec, 0, sizeof(paste_rec));
        paste_rec.type              = UNDO_PASTE;
        paste_rec.row               = match_row;
        paste_rec.col               = match_col;
        paste_rec.end_row           = end_row;
        paste_rec.end_col           = end_col;
        paste_rec.text              = strdup(replacement);
        paste_rec.cursor_row_before = match_row;
        paste_rec.cursor_col_before = match_col;
        paste_rec.cursor_row_after  = end_row;
        paste_rec.cursor_col_after  = end_col;
        undo_push(&buf->undo_stack, paste_rec);

        undo_clear(&buf->redo_stack);

        count++;

        /* Continue from end of the replacement */
        row = end_row;
        col = end_col;

        /*
         * Guard against infinite loops when the replacement is empty
         * (replacing "x" with "" would keep finding "x" at the same spot
         * forever once the string is exhausted — but the search would
         * naturally move forward since the replaced text is gone, so this
         * is only needed when rlen == 0 AND the replacement is the same
         * as the query, which can't happen with an empty replacement).
         *
         * Still, if qlen == 0 somehow, advance to avoid hanging.
         */
        if (qlen == 0) col++;
    }

    free(query);
    free(replacement);

    if (count > 0) {
        /* Update search highlight to show the query on screen */
        ed->search_match_row = -1;
        ed->search_match_col = -1;
        ed->cursor_row  = row;
        ed->cursor_col  = col;
        ed->desired_col = col;
        editor_scroll(ed);
        editor_set_status(ed, "Replaced %d occurrence%s.",
                          count, count == 1 ? "" : "s");
    } else {
        editor_set_status(ed, "Not found: %s", ed->search_query);
    }
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
    memset(&rec, 0, sizeof(rec));
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
    memset(&rec, 0, sizeof(rec));
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
 * Region highlight
 * ============================================================================ */

void editor_mark_region(Editor *ed)
{
    if (ed->sel_active) {
        /*
         * Capture the selection's row range as the region.
         *
         * The anchor (sel_anchor_row) may be above OR below the cursor,
         * depending on which direction the user extended the selection.
         * We normalise so that region_start_row is always the earlier row.
         */
        int sr, er;
        if (ed->sel_anchor_row <= ed->cursor_row) {
            sr = ed->sel_anchor_row;
            er = ed->cursor_row;
        } else {
            sr = ed->cursor_row;
            er = ed->sel_anchor_row;
        }

        ed->region_active    = 1;
        ed->region_start_row = sr;
        ed->region_end_row   = er;

        /*
         * Clear the selection so it does not compete visually with the
         * region border.  The region itself is the persistent indicator.
         */
        editor_selection_clear(ed);

        editor_set_status(ed, "Region marked: lines %d-%d  (Ctrl+U to clear)",
                          sr + 1, er + 1);

    } else if (ed->region_active) {
        /* Second Ctrl+U with no selection: clear the region. */
        ed->region_active = 0;
        editor_set_status(ed, "Region cleared.");

    } else {
        editor_set_status(ed,
            "Select rows with Shift+Arrow, then Ctrl+U to mark a region.");
    }
}

/* ============================================================================
 * Git status panel
 * ============================================================================ */

void editor_toggle_git_panel(Editor *ed)
{
    if (!ed->show_git_panel) {
        /*
         * Panel is hidden → show it.
         * Find the repo root from the current buffer's git state.
         * If the buffer has no filename (e.g. opened with no arguments),
         * git_state.repo_root will be NULL.  Fall back to detecting the
         * repo from the current working directory.
         */
        Buffer *buf = editor_current_buffer(ed);
        const char *root = NULL;
        char *cwd_root = NULL;   /* heap-allocated fallback, freed below */

        if (buf)
            root = buf->git_state.repo_root;

        if (!root || root[0] == '\0') {
            cwd_root = git_find_repo_root(NULL);
            root = cwd_root;
        }

        if (!root || root[0] == '\0') {
            free(cwd_root);
            editor_set_status(ed, "Not in a git repository.");
            return;
        }

        /* Allocate the status list on first use */
        if (!ed->git_status) {
            ed->git_status = malloc(sizeof(GitStatusList));
            if (!ed->git_status) {
                free(cwd_root);
                editor_set_status(ed, "Error: out of memory");
                return;
            }
            memset(ed->git_status, 0, sizeof(GitStatusList));
        }

        /* Refresh the status list */
        git_status_refresh(ed->git_status, root);
        free(cwd_root);  /* safe even if NULL */

        ed->show_git_panel = 1;
        ed->git_panel_focus = 1;
        ed->git_panel_cursor = 0;
        ed->git_panel_scroll = 0;

        if (ed->git_status->count == 0)
            editor_set_status(ed, "Working tree clean — no changes.");
        else
            editor_set_status(ed, "%d changed file(s). Esc to return to editor.",
                              ed->git_status->count);

    } else if (ed->git_panel_focus) {
        /*
         * Panel is visible and has focus → return focus to editor.
         * The panel stays visible.
         */
        ed->git_panel_focus = 0;

    } else {
        /*
         * Panel is visible but editor has focus → give focus back to panel
         * and refresh the status.
         */
        Buffer *buf = editor_current_buffer(ed);
        const char *rroot = (buf) ? buf->git_state.repo_root : NULL;
        char *cwd_fallback = NULL;
        if (!rroot || rroot[0] == '\0') {
            cwd_fallback = git_find_repo_root(NULL);
            rroot = cwd_fallback;
        }
        if (rroot && ed->git_status)
            git_status_refresh(ed->git_status, rroot);
        free(cwd_fallback);

        ed->git_panel_focus = 1;
    }
}

/* ============================================================================
 * File staging
 * ============================================================================ */

void editor_stage_file(Editor *ed)
{
    Buffer *buf = editor_current_buffer(ed);
    if (!buf) return;

    if (!buf->git_state.repo_root) {
        editor_set_status(ed, "Not in a git repository.");
        return;
    }
    if (!buf->filename) {
        editor_set_status(ed, "Buffer has no filename.");
        return;
    }
    if (buf->dirty) {
        editor_set_status(ed, "Unsaved changes — save (Ctrl+S) before staging.");
        return;
    }

    int result = git_stage_file(buf->git_state.repo_root, buf->filename);
    if (result == 0) {
        /* Extract just the filename for the status message */
        const char *name = buf->filename;
        const char *slash = strrchr(buf->filename, '/');
        if (slash) name = slash + 1;

        editor_set_status(ed, "Staged file: %s", name);

        /* Refresh the git status panel if open */
        if (ed->show_git_panel && ed->git_status
                && buf->git_state.repo_root)
            git_status_refresh(ed->git_status, buf->git_state.repo_root);
    } else {
        editor_set_status(ed, "Failed to stage file.");
    }
}

void editor_stage_panel_file(Editor *ed)
{
    GitStatusList *gs = ed->git_status;
    if (!gs) return;
    if (gs->count <= 0) {
        editor_set_status(ed, "No files to stage.");
        return;
    }
    if (ed->git_panel_cursor < 0 || ed->git_panel_cursor >= gs->count)
        return;
    if (gs->repo_root[0] == '\0') {
        editor_set_status(ed, "No repository root.");
        return;
    }

    /*
     * Copy the path and repo root BEFORE calling git_stage_file or
     * git_status_refresh, because those calls may modify gs->entries.
     */
    char path_copy[1024];
    char root_copy[1024];
    strncpy(path_copy, gs->entries[ed->git_panel_cursor].path,
            sizeof(path_copy) - 1);
    path_copy[sizeof(path_copy) - 1] = '\0';
    strncpy(root_copy, gs->repo_root, sizeof(root_copy) - 1);
    root_copy[sizeof(root_copy) - 1] = '\0';

    /* Build the full path from repo root + relative path */
    char fullpath[2048];
    snprintf(fullpath, sizeof(fullpath), "%s/%s", root_copy, path_copy);

    int result = git_stage_file(root_copy, fullpath);
    if (result == 0) {
        editor_set_status(ed, "Staged: %s", path_copy);

        /* Refresh the panel to reflect the new status */
        git_status_refresh(gs, root_copy);

        /* Clamp cursor if entries changed */
        if (ed->git_panel_cursor >= gs->count && gs->count > 0)
            ed->git_panel_cursor = gs->count - 1;
        if (ed->git_panel_cursor < 0)
            ed->git_panel_cursor = 0;
    } else {
        editor_set_status(ed, "Failed to stage: %s", path_copy);
    }
}

/* ============================================================================
 * Git blame view
 * ============================================================================ */

/*
 * refresh_git_blame — re-fetch blame data for the current buffer.
 *
 * Called when blame is toggled on, and when switching buffers while
 * blame is active.
 */
static void refresh_git_blame(Editor *ed)
{
    git_blame_free(&ed->git_blame);

    if (!ed->show_git_blame) return;

    Buffer *buf = editor_current_buffer(ed);
    if (!buf || !buf->git_state.repo_root || !buf->filename)
        return;

    git_blame_refresh(&ed->git_blame, buf->git_state.repo_root,
                      buf->filename, buf->num_lines);
}

void editor_toggle_git_blame(Editor *ed)
{
    if (!ed->show_git_blame) {
        /* Toggle ON */
        Buffer *buf = editor_current_buffer(ed);
        if (!buf || !buf->git_state.repo_root) {
            editor_set_status(ed, "Not in a git repository.");
            return;
        }
        if (!buf->git_state.is_tracked) {
            editor_set_status(ed, "File is not tracked by git.");
            return;
        }
        if (!buf->filename) {
            editor_set_status(ed, "Buffer has no filename.");
            return;
        }

        ed->show_git_blame = 1;
        refresh_git_blame(ed);

        editor_set_status(ed, "Blame ON (Shift+F9 to toggle off).");
        editor_scroll(ed);  /* text area narrowed */

    } else {
        /* Toggle OFF */
        ed->show_git_blame = 0;
        git_blame_free(&ed->git_blame);
        editor_set_status(ed, "Blame OFF.");
        editor_scroll(ed);  /* text area widened */
    }
}

/* ============================================================================
 * Git commit
 * ============================================================================ */

void editor_git_commit(Editor *ed)
{
    /*
     * get_repo_root returns a heap-allocated string (CWD fallback if the
     * current buffer has no filename).  Must be freed before returning.
     */
    char *root = get_repo_root(ed);
    if (!root) {
        editor_set_status(ed, "Not in a git repository.");
        return;
    }

    /*
     * Check if there are staged changes before prompting for a message.
     * This avoids making the user type a message only to find out there
     * was nothing to commit.
     */
    int has_staged = git_has_staged_changes(root);
    if (has_staged <= 0) {
        editor_set_status(ed,
            "Nothing staged to commit. Use F11 to stage hunks first.");
        free(root);
        return;
    }

    /*
     * Prompt for the commit message.
     *
     * display_prompt() draws an input field in the status bar and returns
     * a heap-allocated string, or NULL if the user pressed Escape.
     */
    char *msg = display_prompt(ed, "Commit message: ");
    if (!msg || msg[0] == '\0') {
        free(msg);
        editor_set_status(ed, "Commit cancelled.");
        free(root);
        return;
    }

    /* Run the commit */
    int result = git_commit(root, msg);
    free(msg);

    if (result != 0) {
        editor_set_status(ed, "Commit failed (pre-commit hook or git error).");
        free(root);
        return;
    }

    editor_set_status(ed, "Committed successfully.");

    /*
     * HEAD has changed — refresh git state for ALL open buffers.
     *
     * After a commit, lines that were changed relative to the OLD HEAD
     * may now be unchanged relative to the NEW HEAD.  So gutter markers
     * need to be recalculated for every buffer, not just the current one.
     */
    for (int i = 0; i < ed->num_buffers; i++) {
        Buffer *b = ed->buffers[i];
        if (b && b->filename) {
            git_refresh(&b->git_state, b->filename, b->num_lines);
        }
    }

    /* Refresh inline diff if it's active */
    if (ed->show_inline_diff)
        refresh_inline_diff(ed);

    /* Refresh the git status panel if it's open */
    if (ed->show_git_panel && ed->git_status)
        git_status_refresh(ed->git_status, root);

    free(root);
}

/* ============================================================================
 * Hunk staging
 * ============================================================================ */

void editor_stage_hunk(Editor *ed)
{
    Buffer *buf = editor_current_buffer(ed);
    if (!buf) return;

    /* Verify we're in a git repo with a tracked, saved file */
    if (!buf->git_state.repo_root) {
        /* No repo root from buffer — might be an unnamed buffer */
        if (!buf->filename) {
            editor_set_status(ed, "Save the file first to use git features.");
        } else {
            editor_set_status(ed, "Not in a git repository.");
        }
        return;
    }
    if (!buf->git_state.is_tracked) {
        editor_set_status(ed, "File is not tracked by git.");
        return;
    }
    if (!buf->filename) {
        editor_set_status(ed, "Buffer has no filename — save first.");
        return;
    }
    if (buf->dirty) {
        editor_set_status(ed, "Unsaved changes — save (Ctrl+S) before staging.");
        return;
    }

    /*
     * Try to stage the hunk at the cursor line.
     *
     * git_stage_hunk_at_line() uses `git diff` (unstaged only), so if the
     * cursor is on a line that is already staged or has no changes, it
     * will return -1.
     */
    int result = git_stage_hunk_at_line(buf->git_state.repo_root,
                                        buf->filename,
                                        ed->cursor_row);
    if (result == 0) {
        editor_set_status(ed, "Hunk staged at line %d.", ed->cursor_row + 1);

        /*
         * Refresh the git status panel if it's open, so the user can see
         * the file's status change (e.g. ' M' → 'M ').
         */
        if (ed->show_git_panel && ed->git_status
                && buf->git_state.repo_root) {
            git_status_refresh(ed->git_status, buf->git_state.repo_root);
        }

        /*
         * Refresh gutter markers and inline diff.
         *
         * Note: gutter markers use `git diff HEAD` which compares working
         * tree to HEAD.  Since staging only changes the index (not HEAD
         * or the working tree), the gutter markers won't change.  But we
         * refresh anyway in case the user expects visual feedback.
         *
         * The inline diff view WILL change if the user staged all unstaged
         * changes for a hunk — the diff relative to HEAD stays the same
         * but the diff used for staging (git diff) would now be empty.
         */
        git_refresh(&buf->git_state, buf->filename, buf->num_lines);
        if (ed->show_inline_diff)
            refresh_inline_diff(ed);

    } else {
        editor_set_status(ed,
            "No unstaged hunk at line %d (already staged or unchanged).",
            ed->cursor_row + 1);
    }
}

/* ============================================================================
 * Inline diff view
 * ============================================================================ */

/*
 * refresh_inline_diff — re-fetch and parse the diff for the current buffer.
 *
 * Called internally whenever the inline diff data may be stale (toggle on,
 * buffer switch, file save).  Clears the old chunks and repopulates them
 * from `git diff HEAD`.
 *
 * Does nothing if show_inline_diff is off.
 */
static void refresh_inline_diff(Editor *ed)
{
    /* Always clear old data first */
    git_diff_chunks_free(&ed->inline_diff);

    if (!ed->show_inline_diff) return;

    Buffer *buf = editor_current_buffer(ed);
    if (!buf || !buf->git_state.repo_root || !buf->filename)
        return;

    /*
     * Fetch the raw diff output by shelling out to git.
     * git_get_diff_text() returns a heap-allocated string or NULL.
     */
    char *diff = git_get_diff_text(buf->git_state.repo_root, buf->filename);
    if (diff) {
        git_extract_chunks(&ed->inline_diff, diff);
        free(diff);
    }
}

void editor_toggle_inline_diff(Editor *ed)
{
    if (!ed->show_inline_diff) {
        /*
         * Toggle ON — verify we're in a git repo with a tracked file,
         * then fetch the diff and parse it into chunks.
         */
        Buffer *buf = editor_current_buffer(ed);
        if (!buf || !buf->git_state.repo_root) {
            editor_set_status(ed, "Not in a git repository.");
            return;
        }
        if (!buf->git_state.is_tracked) {
            editor_set_status(ed, "File is not tracked by git.");
            return;
        }
        if (!buf->filename) {
            editor_set_status(ed, "Buffer has no filename.");
            return;
        }

        ed->show_inline_diff = 1;
        refresh_inline_diff(ed);

        if (ed->inline_diff.count == 0)
            editor_set_status(ed, "Inline diff: no changes from HEAD.");
        else
            editor_set_status(ed, "Inline diff ON — %d change(s). F10 to toggle off.",
                              ed->inline_diff.count);

        /* Scroll may need adjustment since phantom lines take space */
        editor_scroll(ed);

    } else {
        /*
         * Toggle OFF — clear the chunks and turn off phantom rendering.
         */
        ed->show_inline_diff = 0;
        git_diff_chunks_free(&ed->inline_diff);
        editor_set_status(ed, "Inline diff OFF.");
    }
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

/*
 * line_screen_rows — how many screen rows a buffer line of `line_len`
 * characters occupies when word-wrap is active and the text area is
 * `text_cols` columns wide.
 *
 * An empty line still occupies one screen row (you have to be able to put
 * the cursor somewhere on it).
 */
static int line_screen_rows(int line_len, int text_cols)
{
    if (text_cols <= 0) return 1;
    if (line_len == 0) return 1;
    return (line_len + text_cols - 1) / text_cols;
}

void editor_scroll(Editor *ed)
{
    Buffer *buf = editor_current_buffer(ed);
    if (!buf) return;

    int rows = editor_rows(ed);
    int cols = editor_cols(ed);

    if (ed->word_wrap) {
        /*
         * Word-wrap mode: horizontal scroll is irrelevant, view_col stays 0.
         * Vertical scrolling must account for lines that span multiple screen
         * rows.
         *
         * Strategy:
         *   1. If cursor is above the viewport, snap view_row to cursor_row.
         *   2. Count how many screen rows are needed from view_row to the
         *      cursor's screen position.  If the cursor falls below the last
         *      visible row, advance view_row one buffer line at a time until
         *      the cursor fits.
         */
        ed->view_col = 0;

        /* Step 1 — cursor above viewport */
        if (ed->cursor_row < ed->view_row)
            ed->view_row = ed->cursor_row;

        /* Step 2 — cursor below viewport: scroll down until cursor is visible */
        for (;;) {
            /*
             * Count screen rows from view_row up to and including the cursor's
             * wrapped sub-row within cursor_row.
             */
            int screen_used = 0;
            for (int r = ed->view_row; r <= ed->cursor_row; r++) {
                int len = buffer_line_len(buf, r);
                if (r == ed->cursor_row) {
                    /* Only count up to (and including) the cursor's sub-row */
                    screen_used += ed->cursor_col / cols + 1;
                } else {
                    screen_used += line_screen_rows(len, cols);
                }
            }

            if (screen_used <= rows)
                break;  /* cursor is visible — done */

            /* Cursor is below the viewport; advance view_row by one line */
            ed->view_row++;
        }

    } else {
        /*
         * Normal (no wrap) mode: standard viewport scrolling.
         *
         * Vertical: keep cursor_row within [view_row, view_row + rows).
         * Horizontal: keep cursor_col within [view_col, view_col + cols).
         */

        /* Vertical */
        if (ed->cursor_row < ed->view_row)
            ed->view_row = ed->cursor_row;
        if (ed->cursor_row >= ed->view_row + rows)
            ed->view_row = ed->cursor_row - rows + 1;

        /*
         * Inline diff adjustment: phantom lines consume screen rows, so the
         * cursor may be pushed off-screen even though it's within
         * [view_row, view_row + rows).  We scroll down until the cursor's
         * effective screen position (including phantom lines) fits.
         *
         * This loop typically runs 0-2 iterations because phantom lines are
         * sparse.  The guard (view_row <= cursor_row) prevents infinite loops.
         */
        if (ed->show_inline_diff) {
            while (ed->view_row <= ed->cursor_row) {
                int phantoms = git_phantom_lines_in_range(
                    &ed->inline_diff,
                    ed->view_row, ed->cursor_row + 1);
                int cursor_screen = (ed->cursor_row - ed->view_row) + phantoms;
                if (cursor_screen < rows) break;
                ed->view_row++;
            }
        }

        /* Horizontal */
        if (ed->cursor_col < ed->view_col)
            ed->view_col = ed->cursor_col;
        if (ed->cursor_col >= ed->view_col + cols)
            ed->view_col = ed->cursor_col - cols + 1;
    }

    /* Clamp view_row so we never scroll past the end of the file */
    int max_view_row = buf->num_lines - 1;
    if (ed->view_row > max_view_row) ed->view_row = max_view_row;
    if (ed->view_row < 0)           ed->view_row = 0;
    if (ed->view_col < 0)           ed->view_col = 0;
}

/* ============================================================================
 * editor_toggle_filetree — show or hide the file explorer panel (Ctrl+B)
 * ============================================================================ */

/*
 * filetree_focus_current — try to highlight the currently open file in the tree.
 *
 * Calls filetree_select_path() which expands parent directories as needed,
 * rebuilds the flat list, and returns the index of the file.  If found, the
 * filetree cursor is moved there.  If not found (e.g. the buffer is unsaved,
 * or the file is outside the tree root), the cursor stays where it is.
 *
 * This is a static helper — only used inside editor_toggle_filetree().
 */
static void filetree_focus_current(Editor *ed)
{
    if (!ed->filetree) return;

    Buffer *buf = editor_current_buffer(ed);
    if (!buf || !buf->filename || buf->filename[0] == '\0') return;

    int idx = filetree_select_path(ed->filetree, buf->filename);
    if (idx >= 0) {
        ed->filetree_cursor = idx;
    }
}

void editor_toggle_filetree(Editor *ed)
{
    if (!ed->show_filetree) {
        /*
         * CASE 1: Panel is hidden → show it and give the tree keyboard focus.
         *
         * Determine the root directory for the tree.  We use the directory
         * containing the currently open file, falling back to "." if the
         * buffer is unsaved.
         *
         * dirname logic (without the non-portable dirname() function):
         *   1. Copy the full filename into `root`.
         *   2. Find the last '/' with strrchr().
         *   3. Null-terminate there to strip the basename.
         *   If there is no '/', the file is in the working directory → ".".
         */
        char root[1024] = ".";

        Buffer *buf = editor_current_buffer(ed);
        if (buf && buf->filename && buf->filename[0] != '\0') {
            strncpy(root, buf->filename, sizeof(root) - 1);
            root[sizeof(root) - 1] = '\0';

            char *last_slash = strrchr(root, '/');
            if (last_slash) {
                if (last_slash == root)
                    *(last_slash + 1) = '\0'; /* keep leading "/" */
                else
                    *last_slash = '\0';
            } else {
                root[0] = '.';
                root[1] = '\0';
            }
        }

        if (ed->filetree == NULL) {
            ed->filetree = filetree_create(root);
        } else {
            filetree_rebuild(ed->filetree);
        }

        ed->show_filetree  = 1;
        ed->filetree_focus = 1;

        /* Highlight the currently open file (expands its parent dirs) */
        filetree_focus_current(ed);

        /* Clamp cursor in case the tree is empty */
        if (ed->filetree) {
            int max_idx = ed->filetree->count - 1;
            if (max_idx < 0) max_idx = 0;
            if (ed->filetree_cursor > max_idx) ed->filetree_cursor = max_idx;
            if (ed->filetree_cursor < 0)       ed->filetree_cursor = 0;
        }

    } else if (ed->filetree_focus) {
        /*
         * CASE 2: Panel is visible AND the tree has keyboard focus.
         * → Return focus to the editor, keep the panel open.
         *
         * This is the "press Ctrl+B while in the tree" action.
         */
        ed->filetree_focus = 0;

    } else {
        /*
         * CASE 3: Panel is visible AND the editor has keyboard focus.
         * → Give focus to the tree and highlight the currently open file.
         *
         * This replaces the old "second Ctrl+B closes the panel" behaviour.
         * To close the panel, the user first presses Ctrl+B to focus the
         * tree (this case), then Ctrl+B again to return to the editor (case 2).
         * A third press would re-enter case 3, and so on.
         *
         * This three-way cycle means Ctrl+B always does something useful:
         *   hidden  → show + focus tree
         *   focused → unfocus (editor gets keys, panel stays)
         *   unfocused → focus tree again (highlights current file)
         */
        ed->filetree_focus = 1;
        filetree_focus_current(ed);

        if (ed->filetree) {
            int max_idx = ed->filetree->count - 1;
            if (max_idx < 0) max_idx = 0;
            if (ed->filetree_cursor > max_idx) ed->filetree_cursor = max_idx;
            if (ed->filetree_cursor < 0)       ed->filetree_cursor = 0;
        }
    }

    /*
     * Recompute the viewport — FILETREE_WIDTH columns may have appeared or
     * disappeared, changing the effective text width editor_cols() returns.
     */
    editor_scroll(ed);
}
