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
#include "build.h"      /* for build_run, build_load_config, etc. */
#include "finder.h"     /* for finder_collect_files, FinderFile, FinderSymbol */
#include "syntax.h"     /* for syntax_detect_language — used by goto_symbol */
#include "lsp.h"        /* for LspServer, lsp_server_start/stop, etc. */
#include "json.h"       /* for json_parse, json_get, etc. — LSP responses */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <sys/stat.h>  /* for mkdir — used by recent_files_path */
#include <unistd.h>    /* for getpid — used by LSP initialize */

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
    int rows = ed->term_rows - 1 - TAB_BAR_HEIGHT;
    /*
     * When the build panel is visible, it occupies rows at the bottom
     * of the text area (above the status bar).  Subtract its height.
     */
    if (ed->show_build_panel && ed->build_result)
        rows -= BUILD_PANEL_HEIGHT;
    if (rows < 1) rows = 1;
    return rows;
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

    /* Load recent files from disk */
    editor_recent_load(ed);

    /* Initialize themes (built-in defaults + user themes from disk) */
    theme_init(&ed->theme_mgr);
    theme_load_from_disk(&ed->theme_mgr);

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

    /* Free build result */
    if (ed->build_result) {
        build_result_free(ed->build_result);
        free(ed->build_result);
        ed->build_result = NULL;
    }

    /* Stop LSP server if running */
    editor_lsp_stop(ed);

    /* Save and free recent files list */
    editor_recent_save(ed);
    for (int i = 0; i < ed->recent_count; i++)
        free(ed->recent_files[i]);
    ed->recent_count = 0;
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
    if (buf->filename) {
        git_refresh(&buf->git_state, buf->filename, buf->num_lines);
        editor_recent_add(ed, buf->filename);
    }

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
            editor_recent_add(ed, path);
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

    /* Notify LSP server about the save */
    editor_lsp_did_save(ed);

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
 *
 * System clipboard integration
 * ----------------------------
 * In addition to the internal clipboard (ed->clipboard), we also push/pull
 * to the system clipboard so copy/paste works between texty and other apps.
 *
 * On macOS: pbcopy / pbpaste
 * On Linux: xclip -selection clipboard
 *
 * If the system clipboard command is not available, we silently fall back to
 * the internal clipboard only.
 * ============================================================================ */

/*
 * clipboard_to_system — push the internal clipboard to the OS clipboard.
 *
 * Uses popen() to pipe text into pbcopy (macOS) or xclip (Linux).
 * Silently does nothing if the command is not available.
 */
static void clipboard_to_system(const char *text)
{
    if (!text || !text[0]) return;

    /*
     * Try pbcopy first (macOS), then xclip (Linux).
     * popen() returns NULL if the command cannot be executed.
     */
    FILE *fp = popen("pbcopy 2>/dev/null", "w");
    if (!fp)
        fp = popen("xclip -selection clipboard 2>/dev/null", "w");
    if (!fp) return;

    fwrite(text, 1, strlen(text), fp);
    pclose(fp);
}

/*
 * clipboard_from_system — read the OS clipboard into a heap-allocated string.
 *
 * Uses popen() to read from pbpaste (macOS) or xclip (Linux).
 * Returns a heap-allocated string (caller must free), or NULL if the
 * system clipboard is empty or the command is not available.
 */
static char *clipboard_from_system(void)
{
    FILE *fp = popen("pbpaste 2>/dev/null", "r");
    if (!fp)
        fp = popen("xclip -selection clipboard -o 2>/dev/null", "r");
    if (!fp) return NULL;

    /*
     * Read the clipboard contents into a dynamically growing buffer.
     * Start with 4 KB and double as needed.
     */
    int cap = 4096;
    int len = 0;
    char *buf = malloc(cap);
    if (!buf) { pclose(fp); return NULL; }

    int ch;
    while ((ch = fgetc(fp)) != EOF) {
        if (len + 1 >= cap) {
            cap *= 2;
            char *tmp = realloc(buf, cap);
            if (!tmp) { free(buf); pclose(fp); return NULL; }
            buf = tmp;
        }
        buf[len++] = (char)ch;
    }
    pclose(fp);

    if (len == 0) { free(buf); return NULL; }
    buf[len] = '\0';
    return buf;
}

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

    /* Also push to the system clipboard for cross-app copy/paste */
    clipboard_to_system(ed->clipboard);

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

    /* Also push to the system clipboard for cross-app copy/paste */
    clipboard_to_system(ed->clipboard);

    editor_set_status(ed, "Cut.");
}

void editor_paste(Editor *ed)
{
    Buffer *buf = editor_current_buffer(ed);
    if (!buf) return;

    /*
     * If the internal clipboard is empty, try the system clipboard.
     * This lets the user paste text copied from other applications.
     */
    if (!ed->clipboard) {
        ed->clipboard = clipboard_from_system();
    }
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
 * Recent files
 * ============================================================================ */

void editor_recent_add(Editor *ed, const char *path)
{
    if (!path || path[0] == '\0') return;

    /*
     * If the path is already in the list, remove it from its current
     * position so we can re-insert it at the front (most recent first).
     */
    int existing = -1;
    for (int i = 0; i < ed->recent_count; i++) {
        if (ed->recent_files[i] && strcmp(ed->recent_files[i], path) == 0) {
            existing = i;
            break;
        }
    }

    if (existing >= 0) {
        /* Move to front: free the slot, shift entries down */
        char *saved = ed->recent_files[existing];
        for (int i = existing; i > 0; i--)
            ed->recent_files[i] = ed->recent_files[i - 1];
        ed->recent_files[0] = saved;
        return;
    }

    /*
     * Not in the list — insert at front.
     * If the list is full, free the last entry.
     */
    if (ed->recent_count >= RECENT_FILES_MAX) {
        free(ed->recent_files[RECENT_FILES_MAX - 1]);
        ed->recent_count = RECENT_FILES_MAX - 1;
    }

    /* Shift all entries down by one */
    for (int i = ed->recent_count; i > 0; i--)
        ed->recent_files[i] = ed->recent_files[i - 1];

    ed->recent_files[0] = strdup(path);
    ed->recent_count++;
}

/*
 * recent_files_path — get the path to the recent files persistence file.
 *
 * Returns a heap-allocated path to ~/.config/texty/recent_files,
 * creating the directory if needed.  Returns NULL on failure.
 */
static char *recent_files_path(void)
{
    const char *home = getenv("HOME");
    if (!home) return NULL;

    char dir[1024];
    snprintf(dir, sizeof(dir), "%s/.config/texty", home);

    /* Create directory if it doesn't exist (mkdir -p equivalent) */
    char parent[1024];
    snprintf(parent, sizeof(parent), "%s/.config", home);
    mkdir(parent, 0755);  /* ignore error if exists */
    mkdir(dir, 0755);     /* ignore error if exists */

    char *path = malloc(1024);
    if (!path) return NULL;
    snprintf(path, 1024, "%s/recent_files", dir);
    return path;
}

void editor_recent_load(Editor *ed)
{
    char *path = recent_files_path();
    if (!path) return;

    FILE *fp = fopen(path, "r");
    free(path);
    if (!fp) return;

    /*
     * Each line in the file is an absolute file path.
     * Read up to RECENT_FILES_MAX lines.
     */
    char line[1024];
    while (ed->recent_count < RECENT_FILES_MAX
           && fgets(line, sizeof(line), fp)) {
        /* Strip trailing newline */
        int len = (int)strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
            line[--len] = '\0';

        if (len > 0)
            ed->recent_files[ed->recent_count++] = strdup(line);
    }

    fclose(fp);
}

void editor_recent_save(Editor *ed)
{
    char *path = recent_files_path();
    if (!path) return;

    FILE *fp = fopen(path, "w");
    free(path);
    if (!fp) return;

    for (int i = 0; i < ed->recent_count; i++) {
        if (ed->recent_files[i])
            fprintf(fp, "%s\n", ed->recent_files[i]);
    }

    fclose(fp);
}

void editor_recent_files(Editor *ed)
{
    if (ed->recent_count == 0) {
        editor_set_status(ed, "No recent files.");
        return;
    }

    /*
     * Build a FinderFile array from the recent list so we can reuse
     * the finder popup UI.  The display path is the basename or a
     * shortened path for readability.
     */
    FinderFile *files = malloc(ed->recent_count * sizeof(FinderFile));
    if (!files) return;

    int count = 0;
    for (int i = 0; i < ed->recent_count; i++) {
        if (!ed->recent_files[i]) continue;

        FinderFile *f = &files[count];
        strncpy(f->path, ed->recent_files[i], sizeof(f->path) - 1);
        f->path[sizeof(f->path) - 1] = '\0';

        /*
         * For display, use the filename portion or a short relative-ish path.
         * We try to show the last 2 path components for context.
         */
        const char *disp = ed->recent_files[i];
        /* Find the second-to-last '/' */
        const char *slash = strrchr(disp, '/');
        if (slash && slash > disp) {
            const char *prev = slash - 1;
            while (prev > disp && *prev != '/') prev--;
            if (*prev == '/') prev++;
            disp = prev;
        }
        strncpy(f->display, disp, sizeof(f->display) - 1);
        f->display[sizeof(f->display) - 1] = '\0';

        count++;
    }

    if (count == 0) {
        free(files);
        editor_set_status(ed, "No recent files.");
        return;
    }

    /* Show the picker popup (reuses the fuzzy finder UI) */
    char *selected = display_finder_popup(ed, files, count);

    if (selected) {
        editor_open_or_switch(ed, selected);
        free(selected);
    }

    free(files);
    display_render(ed);
}

/* ============================================================================
 * Go-to-symbol
 * ============================================================================ */

void editor_goto_symbol(Editor *ed)
{
    Buffer *buf = editor_current_buffer(ed);
    if (!buf || buf->num_lines == 0) {
        editor_set_status(ed, "No file open.");
        return;
    }

    /*
     * Build an array of line pointers for the symbol extractor.
     * buffer_get_line() returns a const char* for each line.
     */
    const char **lines = malloc(buf->num_lines * sizeof(const char *));
    if (!lines) return;
    for (int i = 0; i < buf->num_lines; i++)
        lines[i] = buffer_get_line(buf, i);

    /* Detect language for language-specific patterns */
    int lang = (int)syntax_detect_language(buf->filename);

    /* Extract symbols */
    FinderSymbol *syms = malloc(FINDER_MAX_SYMBOLS * sizeof(FinderSymbol));
    if (!syms) { free(lines); return; }

    int num_syms = finder_extract_symbols(lines, buf->num_lines, lang,
                                          syms, FINDER_MAX_SYMBOLS);
    free(lines);

    if (num_syms == 0) {
        free(syms);
        editor_set_status(ed, "No symbols found in this file.");
        return;
    }

    /*
     * Build FinderFile entries from symbols so we can reuse the popup.
     * display = "kind name :line"
     * path = line number as string (we'll parse it back on selection)
     */
    FinderFile *files = malloc(num_syms * sizeof(FinderFile));
    if (!files) { free(syms); return; }

    for (int i = 0; i < num_syms; i++) {
        const char *kind_str = "?";
        switch (syms[i].kind) {
            case 'f': kind_str = "fn"; break;
            case 's': kind_str = "struct"; break;
            case 'e': kind_str = "enum"; break;
            case 'd': kind_str = "define"; break;
            case 'c': kind_str = "class"; break;
            case 't': kind_str = "typedef"; break;
            case 'm': kind_str = "method"; break;
        }
        snprintf(files[i].display, sizeof(files[i].display),
                 "%-6s %s :%d", kind_str, syms[i].name, syms[i].line);
        snprintf(files[i].path, sizeof(files[i].path),
                 "%d", syms[i].line);
    }

    char *selected = display_finder_popup(ed, files, num_syms);

    if (selected) {
        /* Parse the line number from the path field */
        int target_line = atoi(selected);
        free(selected);

        if (target_line > 0) {
            ed->cursor_row = target_line - 1;
            if (ed->cursor_row >= buf->num_lines)
                ed->cursor_row = buf->num_lines - 1;
            ed->cursor_col  = 0;
            ed->desired_col = 0;
            editor_scroll(ed);
        }
    }

    free(files);
    free(syms);
    display_render(ed);
}

void editor_goto_workspace_symbol(Editor *ed)
{
    /*
     * Determine the project root and collect all source files.
     */
    char *root = get_repo_root(ed);
    if (!root) {
        Buffer *buf = editor_current_buffer(ed);
        if (buf && buf->filename) {
            char buf_dir[2048];
            strncpy(buf_dir, buf->filename, sizeof(buf_dir) - 1);
            buf_dir[sizeof(buf_dir) - 1] = '\0';
            char *slash = strrchr(buf_dir, '/');
            if (slash) *slash = '\0';
            else strcpy(buf_dir, ".");
            root = strdup(buf_dir);
        } else {
            root = strdup(".");
        }
    }

    FinderFile *project_files = malloc(FINDER_MAX_FILES * sizeof(FinderFile));
    if (!project_files) { free(root); return; }

    int num_project = finder_collect_files(root, project_files, FINDER_MAX_FILES);

    if (num_project == 0) {
        free(project_files);
        free(root);
        editor_set_status(ed, "No files found in project.");
        return;
    }

    /*
     * Scan each file for symbols.  For performance, we read each file,
     * extract symbols, and accumulate results.  We limit total symbols
     * to keep things manageable.
     */
    editor_set_status(ed, "Scanning project for symbols...");
    display_render(ed);

    int max_total = FINDER_MAX_SYMBOLS;
    FinderFile *sym_files = malloc(max_total * sizeof(FinderFile));
    if (!sym_files) { free(project_files); free(root); return; }

    FinderSymbol syms_buf[64];  /* per-file temporary buffer */
    int total_syms = 0;

    for (int f = 0; f < num_project && total_syms < max_total; f++) {
        /* Read the file */
        FILE *fp = fopen(project_files[f].path, "r");
        if (!fp) continue;

        /* Read lines into a temporary array */
        char *file_lines[4096];
        int num_lines = 0;
        char line_buf[1024];
        while (num_lines < 4096 && fgets(line_buf, sizeof(line_buf), fp)) {
            /* Strip trailing newline */
            int len = (int)strlen(line_buf);
            if (len > 0 && line_buf[len - 1] == '\n') line_buf[len - 1] = '\0';
            file_lines[num_lines] = strdup(line_buf);
            num_lines++;
        }
        fclose(fp);

        /* Detect language and extract symbols */
        int lang = (int)syntax_detect_language(project_files[f].display);
        int n = finder_extract_symbols((const char **)file_lines, num_lines,
                                       lang, syms_buf, 64);

        /* Add to the combined list */
        for (int s = 0; s < n && total_syms < max_total; s++) {
            const char *kind_str = "?";
            switch (syms_buf[s].kind) {
                case 'f': kind_str = "fn"; break;
                case 's': kind_str = "struct"; break;
                case 'e': kind_str = "enum"; break;
                case 'd': kind_str = "define"; break;
                case 'c': kind_str = "class"; break;
            }
            snprintf(sym_files[total_syms].display,
                     sizeof(sym_files[total_syms].display),
                     "%-6s %s  (%s:%d)",
                     kind_str, syms_buf[s].name,
                     project_files[f].display, syms_buf[s].line);
            snprintf(sym_files[total_syms].path,
                     sizeof(sym_files[total_syms].path),
                     "%s:%d", project_files[f].path, syms_buf[s].line);
            total_syms++;
        }

        /* Free temporary line copies */
        for (int l = 0; l < num_lines; l++)
            free(file_lines[l]);
    }

    free(project_files);
    free(root);

    if (total_syms == 0) {
        free(sym_files);
        editor_set_status(ed, "No symbols found in project.");
        return;
    }

    char *selected = display_finder_popup(ed, sym_files, total_syms);

    if (selected) {
        /*
         * Parse "filepath:line" from the path field.
         * Find the last colon to split path from line number.
         */
        char *colon = strrchr(selected, ':');
        if (colon) {
            *colon = '\0';
            int target_line = atoi(colon + 1);

            editor_open_or_switch(ed, selected);

            Buffer *buf = editor_current_buffer(ed);
            if (buf && target_line > 0) {
                ed->cursor_row = target_line - 1;
                if (ed->cursor_row >= buf->num_lines)
                    ed->cursor_row = buf->num_lines - 1;
                ed->cursor_col  = 0;
                ed->desired_col = 0;
                editor_scroll(ed);
            }
        }
        free(selected);
    }

    free(sym_files);
    display_render(ed);
}

/* ============================================================================
 * Command palette
 * ============================================================================ */

/*
 * Command palette entry — a command name + key binding + action ID.
 *
 * The action ID is used to dispatch the command when selected.
 * We use a simple enum-like integer.
 */
#define CMD_SAVE             1
#define CMD_QUIT             2
#define CMD_NEW_BUFFER       3
#define CMD_OPEN_FILE        4
#define CMD_CLOSE_BUFFER     5
#define CMD_FIND             6
#define CMD_FIND_NEXT        7
#define CMD_FIND_PREV        8
#define CMD_REPLACE          9
#define CMD_GOTO_LINE       10
#define CMD_UNDO            11
#define CMD_REDO            12
#define CMD_COPY            13
#define CMD_CUT             14
#define CMD_PASTE           15
#define CMD_SELECT_ALL      16
#define CMD_TOGGLE_TREE     17
#define CMD_TOGGLE_WHITESPACE 18
#define CMD_TOGGLE_WORDWRAP 19
#define CMD_BUILD           20
#define CMD_GIT_PANEL       21
#define CMD_GIT_BLAME       22
#define CMD_INLINE_DIFF     23
#define CMD_STAGE_HUNK      24
#define CMD_GIT_COMMIT      25
#define CMD_FUZZY_FIND      26
#define CMD_RECENT_FILES    27
#define CMD_GOTO_SYMBOL     28
#define CMD_WORKSPACE_SYMBOL 29
#define CMD_NEXT_BUFFER     30
#define CMD_PREV_BUFFER     31
#define CMD_MARK_REGION     32
#define CMD_COMMAND_PALETTE 33
#define CMD_CYCLE_THEME    34
#define CMD_LSP_COMPLETE   35
#define CMD_LSP_GOTO_DEF   36
#define CMD_LSP_HOVER      37
#define CMD_LSP_REFERENCES 38
#define CMD_LSP_FORMAT     39
#define CMD_LSP_RENAME     40
#define CMD_LSP_SIGNATURE  41

typedef struct {
    const char *name;
    const char *keys;
    int         action;
} PaletteEntry;

static const PaletteEntry palette_commands[] = {
    { "Save",                   "Ctrl+S",     CMD_SAVE },
    { "Quit",                   "Ctrl+Q",     CMD_QUIT },
    { "New Buffer",             "Ctrl+N",     CMD_NEW_BUFFER },
    { "Open File",              "Ctrl+O",     CMD_OPEN_FILE },
    { "Close Buffer",           "Ctrl+W",     CMD_CLOSE_BUFFER },
    { "Next Buffer",            "Ctrl+]",     CMD_NEXT_BUFFER },
    { "Previous Buffer",        "Ctrl+\\",    CMD_PREV_BUFFER },
    { "Find",                   "Ctrl+F",     CMD_FIND },
    { "Find Next",              "F3",         CMD_FIND_NEXT },
    { "Find Previous",          "Shift+F3",   CMD_FIND_PREV },
    { "Replace All",            "Ctrl+R",     CMD_REPLACE },
    { "Go to Line",             "Ctrl+G",     CMD_GOTO_LINE },
    { "Undo",                   "Ctrl+Z",     CMD_UNDO },
    { "Redo",                   "Ctrl+Y",     CMD_REDO },
    { "Copy",                   "Ctrl+C",     CMD_COPY },
    { "Cut",                    "Ctrl+X",     CMD_CUT },
    { "Paste",                  "Ctrl+V",     CMD_PASTE },
    { "Select All",             "Ctrl+A",     CMD_SELECT_ALL },
    { "Toggle File Explorer",   "Ctrl+B",     CMD_TOGGLE_TREE },
    { "Toggle Whitespace",      "F2",         CMD_TOGGLE_WHITESPACE },
    { "Toggle Word Wrap",       "F4",         CMD_TOGGLE_WORDWRAP },
    { "Build",                  "F5",         CMD_BUILD },
    { "Go to Symbol",           "F7",         CMD_GOTO_SYMBOL },
    { "Command Palette",        "F8",         CMD_COMMAND_PALETTE },
    { "Git Status Panel",       "F9",         CMD_GIT_PANEL },
    { "Git Blame",              "Shift+F9",   CMD_GIT_BLAME },
    { "Inline Diff",            "F10",        CMD_INLINE_DIFF },
    { "Stage Hunk",             "F11",        CMD_STAGE_HUNK },
    { "Git Commit",             "F12",        CMD_GIT_COMMIT },
    { "Fuzzy File Finder",      "Ctrl+P",     CMD_FUZZY_FIND },
    { "Recent Files",           "Ctrl+E",     CMD_RECENT_FILES },
    { "Workspace Symbol",       "Ctrl+T",     CMD_WORKSPACE_SYMBOL },
    { "Mark Region",            "Ctrl+U",     CMD_MARK_REGION },
    { "Cycle Theme",            "F6",         CMD_CYCLE_THEME },
    { "LSP Completion",         "Ctrl+Space", CMD_LSP_COMPLETE },
    { "Go to Definition",       "F1",         CMD_LSP_GOTO_DEF },
    { "Hover Documentation",    "Ctrl+K",     CMD_LSP_HOVER },
    { "Find All References",    "F8 →",       CMD_LSP_REFERENCES },
    { "Format Document",        "F8 →",       CMD_LSP_FORMAT },
    { "Rename Symbol",          "F8 →",       CMD_LSP_RENAME },
    { "Signature Help",         "F8 →",       CMD_LSP_SIGNATURE },
    { NULL, NULL, 0 }
};

void editor_command_palette(Editor *ed)
{
    /* Count commands */
    int num_cmds = 0;
    while (palette_commands[num_cmds].name) num_cmds++;

    /* Build FinderFile entries: display = "Name  (Key)", path = action ID */
    FinderFile *files = malloc(num_cmds * sizeof(FinderFile));
    if (!files) return;

    for (int i = 0; i < num_cmds; i++) {
        snprintf(files[i].display, sizeof(files[i].display),
                 "%-25s  %s",
                 palette_commands[i].name,
                 palette_commands[i].keys);
        snprintf(files[i].path, sizeof(files[i].path),
                 "%d", palette_commands[i].action);
    }

    char *selected = display_finder_popup(ed, files, num_cmds);
    free(files);

    if (!selected) {
        display_render(ed);
        return;
    }

    int action = atoi(selected);
    free(selected);

    /* Redraw before dispatching (clear the popup) */
    display_render(ed);

    /* Dispatch the selected command */
    switch (action) {
    case CMD_SAVE:              editor_save(ed); break;
    case CMD_QUIT:              ed->should_quit = 1; break;
    case CMD_NEW_BUFFER:        editor_new_buffer(ed); break;
    case CMD_OPEN_FILE:         { char *p = display_prompt(ed, "Open file: ");
                                  if (p && p[0]) editor_open_file(ed, p);
                                  free(p); break; }
    case CMD_CLOSE_BUFFER:      editor_close_buffer(ed); break;
    case CMD_NEXT_BUFFER:       editor_next_buffer(ed); break;
    case CMD_PREV_BUFFER:       editor_prev_buffer(ed); break;
    case CMD_FIND:              editor_find(ed); break;
    case CMD_FIND_NEXT:         editor_find_next(ed); break;
    case CMD_FIND_PREV:         editor_find_prev(ed); break;
    case CMD_REPLACE:           editor_replace(ed); break;
    case CMD_GOTO_LINE:         editor_goto_line(ed); break;
    case CMD_UNDO:              editor_undo(ed); break;
    case CMD_REDO:              editor_redo(ed); break;
    case CMD_COPY:              editor_copy(ed); break;
    case CMD_CUT:               editor_cut(ed); break;
    case CMD_PASTE:             editor_paste(ed); break;
    case CMD_SELECT_ALL:        editor_select_all(ed); break;
    case CMD_TOGGLE_TREE:       editor_toggle_filetree(ed); break;
    case CMD_TOGGLE_WHITESPACE: editor_toggle_whitespace(ed); break;
    case CMD_TOGGLE_WORDWRAP:   editor_toggle_word_wrap(ed); break;
    case CMD_BUILD:             editor_build(ed); break;
    case CMD_GOTO_SYMBOL:       editor_goto_symbol(ed); break;
    case CMD_GIT_PANEL:         editor_toggle_git_panel(ed); break;
    case CMD_GIT_BLAME:         editor_toggle_git_blame(ed); break;
    case CMD_INLINE_DIFF:       editor_toggle_inline_diff(ed); break;
    case CMD_STAGE_HUNK:        editor_stage_hunk(ed); break;
    case CMD_GIT_COMMIT:        editor_git_commit(ed); break;
    case CMD_FUZZY_FIND:        editor_fuzzy_find(ed); break;
    case CMD_RECENT_FILES:      editor_recent_files(ed); break;
    case CMD_WORKSPACE_SYMBOL:  editor_goto_workspace_symbol(ed); break;
    case CMD_MARK_REGION:       editor_mark_region(ed); break;
    case CMD_CYCLE_THEME:       {
        const char *nm = theme_cycle(&ed->theme_mgr);
        display_apply_theme(theme_active(&ed->theme_mgr));
        editor_set_status(ed, "Theme: %s", nm);
        break;
    }
    case CMD_LSP_COMPLETE:      editor_lsp_complete(ed); break;
    case CMD_LSP_GOTO_DEF:      editor_lsp_goto_definition(ed); break;
    case CMD_LSP_HOVER:         editor_lsp_hover(ed); break;
    case CMD_LSP_REFERENCES:    editor_lsp_references(ed); break;
    case CMD_LSP_FORMAT:        editor_lsp_format(ed); break;
    case CMD_LSP_RENAME:        editor_lsp_rename(ed); break;
    case CMD_LSP_SIGNATURE:     editor_lsp_signature_help(ed); break;
    }
}

/* ============================================================================
 * Fuzzy file finder
 * ============================================================================ */

void editor_fuzzy_find(Editor *ed)
{
    /*
     * Determine the project root to scan.
     * Same fallback chain as other features: git repo root → buffer dir → CWD.
     */
    char *root = get_repo_root(ed);
    char buf_dir[2048] = "";

    if (!root) {
        Buffer *buf = editor_current_buffer(ed);
        if (buf && buf->filename) {
            strncpy(buf_dir, buf->filename, sizeof(buf_dir) - 1);
            buf_dir[sizeof(buf_dir) - 1] = '\0';
            char *slash = strrchr(buf_dir, '/');
            if (slash) *slash = '\0';
            else strcpy(buf_dir, ".");
            root = strdup(buf_dir);
        } else {
            root = strdup(".");
        }
    }

    if (!root) {
        editor_set_status(ed, "Error: could not determine project root.");
        return;
    }

    /*
     * Collect all files in the project.
     * This heap allocation (~16 MB) is freed at the end of this function.
     */
    FinderFile *files = malloc(FINDER_MAX_FILES * sizeof(FinderFile));
    if (!files) {
        free(root);
        editor_set_status(ed, "Error: out of memory.");
        return;
    }

    int num_files = finder_collect_files(root, files, FINDER_MAX_FILES);
    free(root);

    if (num_files == 0) {
        editor_set_status(ed, "No files found in project.");
        free(files);
        return;
    }

    /*
     * Show the finder popup (modal — blocks until user selects or cancels).
     * display_finder_popup() handles its own input loop and rendering.
     */
    char *selected = display_finder_popup(ed, files, num_files);

    if (selected) {
        editor_open_or_switch(ed, selected);
        free(selected);
    }

    free(files);

    /* Redraw the full screen to clear the popup overlay */
    display_render(ed);
}

/* ============================================================================
 * Build system
 * ============================================================================ */

void editor_build(Editor *ed)
{
    /*
     * Determine the working directory for the build.
     * Use the git repo root if available, otherwise the directory of the
     * current buffer's file, otherwise CWD.
     */
    char *root = get_repo_root(ed);
    const char *work_dir = root;
    char buf_dir[2048] = "";

    if (!work_dir) {
        Buffer *buf = editor_current_buffer(ed);
        if (buf && buf->filename) {
            strncpy(buf_dir, buf->filename, sizeof(buf_dir) - 1);
            buf_dir[sizeof(buf_dir) - 1] = '\0';
            char *slash = strrchr(buf_dir, '/');
            if (slash) *slash = '\0';
            else strcpy(buf_dir, ".");
            work_dir = buf_dir;
        } else {
            work_dir = ".";
        }
    }

    /*
     * Load the build command from texty.json on first invocation.
     * If no config file is found, defaults to "make".
     */
    if (ed->build_command[0] == '\0')
        build_load_config(ed->build_command, sizeof(ed->build_command),
                          work_dir);

    /* Allocate the build result on first use */
    if (!ed->build_result) {
        ed->build_result = malloc(sizeof(BuildResult));
        if (!ed->build_result) {
            free(root);
            editor_set_status(ed, "Error: out of memory");
            return;
        }
        memset(ed->build_result, 0, sizeof(BuildResult));
    }

    /*
     * Show "Building..." before the blocking popen call so the user
     * knows the editor hasn't frozen.
     */
    editor_set_status(ed, "Building: %s ...", ed->build_command);
    display_render(ed);

    /* Run the build (this blocks until the command finishes) */
    int result = build_run(ed->build_result, ed->build_command, work_dir);
    free(root);

    if (result != 0) {
        editor_set_status(ed, "Build command failed to execute.");
        return;
    }

    /* Show the build panel */
    ed->show_build_panel  = 1;
    ed->build_panel_focus = 1;
    ed->build_panel_cursor = 0;
    ed->build_panel_scroll = 0;

    /* Scroll adjustment since text area height changed */
    editor_scroll(ed);

    /* Status message with results */
    int errs = 0, warns = 0;
    for (int i = 0; i < ed->build_result->error_count; i++) {
        if (ed->build_result->errors[i].is_warning) warns++;
        else errs++;
    }

    if (ed->build_result->exit_status == 0)
        editor_set_status(ed, "Build succeeded. %d warning(s).", warns);
    else
        editor_set_status(ed, "Build failed: %d error(s), %d warning(s).",
                          errs, warns);
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
 * LSP
 * ============================================================================ */

/*
 * lsp_language_id — map a SyntaxLang to an LSP languageId string.
 *
 * Returns NULL if the language is not supported for LSP.
 */
static const char *lsp_language_id(int lang)
{
    switch (lang) {
        case 1: return "c";          /* LANG_C */
        case 2: return "python";     /* LANG_PYTHON */
        case 3: return "javascript"; /* LANG_JS */
        case 4: return "rust";       /* LANG_RUST */
        case 5: return "go";         /* LANG_GO */
        default: return NULL;
    }
}

/*
 * lsp_find_server_command — look up the LSP server command for a language.
 *
 * Reads texty.json's "lsp_servers" object.  Returns a heap-allocated
 * command string, or NULL if not configured.
 */
static char *lsp_find_server_command(Editor *ed, const char *lang_id)
{
    if (!lang_id) return NULL;

    /* Determine the project root for finding texty.json */
    char *root = get_repo_root(ed);
    if (!root) return NULL;

    char config_path[2048];
    snprintf(config_path, sizeof(config_path), "%s/texty.json", root);

    FILE *fp = fopen(config_path, "r");
    if (!fp) { free(root); return NULL; }

    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (size <= 0 || size > 8192) { fclose(fp); free(root); return NULL; }

    char *json_text = malloc(size + 1);
    if (!json_text) { fclose(fp); free(root); return NULL; }
    int read_n = (int)fread(json_text, 1, size, fp);
    fclose(fp);
    json_text[read_n] = '\0';
    free(root);

    /* Parse the JSON */
    JsonValue *config = json_parse(json_text, -1);
    free(json_text);
    if (!config) return NULL;

    /* Look up lsp_servers.lang_id */
    JsonValue *servers = json_get(config, "lsp_servers");
    if (!servers) { json_free(config); return NULL; }

    const char *cmd = json_get_string(json_get(servers, lang_id));
    char *result = cmd ? strdup(cmd) : NULL;

    json_free(config);
    return result;
}

void editor_lsp_start(Editor *ed)
{
    if (ed->lsp_server) return;  /* already running */

    Buffer *buf = editor_current_buffer(ed);
    if (!buf || !buf->filename) return;

    /* Detect language and find server command */
    int lang = (int)syntax_detect_language(buf->filename);
    const char *lang_id = lsp_language_id(lang);
    if (!lang_id) return;  /* unsupported language */

    char *cmd = lsp_find_server_command(ed, lang_id);
    if (!cmd) return;  /* no server configured */

    /* Determine workspace root */
    char *root = get_repo_root(ed);
    if (!root) root = strdup(".");

    editor_set_status(ed, "LSP: starting %s...", cmd);
    display_render(ed);

    /* Start the server process */
    ed->lsp_server = lsp_server_start(cmd, root);
    free(cmd);

    if (!ed->lsp_server) {
        editor_set_status(ed, "LSP: failed to start server.");
        free(root);
        return;
    }

    /*
     * Send the initialize request.
     *
     * The initialize request tells the server about our capabilities
     * and the workspace root.  The server responds with its capabilities.
     */
    char params[4096];
    char root_uri[2048];
    lsp_path_to_uri(root, root_uri, sizeof(root_uri));
    free(root);

    snprintf(params, sizeof(params),
        "{"
        "  \"processId\": %d,"
        "  \"rootUri\": \"%s\","
        "  \"capabilities\": {"
        "    \"textDocument\": {"
        "      \"completion\": {\"completionItem\": {\"snippetSupport\": false}},"
        "      \"synchronization\": {\"didSave\": true},"
        "      \"hover\": {},"
        "      \"definition\": {},"
        "      \"publishDiagnostics\": {}"
        "    }"
        "  }"
        "}",
        (int)getpid(), root_uri);

    lsp_send_request(ed->lsp_server, "initialize", params);
    editor_set_status(ed, "LSP: initializing...");
}

void editor_lsp_stop(Editor *ed)
{
    if (!ed->lsp_server) return;

    /*
     * Send shutdown request, then exit notification.
     * lsp_server_stop handles killing the process if needed.
     */
    lsp_send_request(ed->lsp_server, "shutdown", "null");
    lsp_send_notification(ed->lsp_server, "exit", NULL);

    lsp_server_stop(ed->lsp_server);
    ed->lsp_server = NULL;
    editor_set_status(ed, "LSP: stopped.");
}

void editor_lsp_did_open(Editor *ed)
{
    if (!ed->lsp_server || !ed->lsp_server->initialized) return;

    Buffer *buf = editor_current_buffer(ed);
    if (!buf || !buf->filename) return;

    int lang = (int)syntax_detect_language(buf->filename);
    const char *lang_id = lsp_language_id(lang);
    if (!lang_id) lang_id = "plaintext";

    /* Build the full file content as a JSON-escaped string */
    char uri[2048];
    lsp_path_to_uri(buf->filename, uri, sizeof(uri));

    /*
     * Collect all buffer text into one string for the didOpen notification.
     * We need to JSON-escape it for safe embedding.
     */
    int total_len = 0;
    for (int i = 0; i < buf->num_lines; i++)
        total_len += buffer_line_len(buf, i) + 1;  /* +1 for \n */

    char *text = malloc(total_len + 1);
    if (!text) return;
    int pos = 0;
    for (int i = 0; i < buf->num_lines; i++) {
        const char *line = buffer_get_line(buf, i);
        int len = buffer_line_len(buf, i);
        memcpy(text + pos, line, len);
        pos += len;
        if (i < buf->num_lines - 1)
            text[pos++] = '\n';
    }
    text[pos] = '\0';

    /* Escape the text for JSON */
    int esc_size = pos * 4 + 1;  /* worst case: every char → \uXXXX */
    char *escaped = malloc(esc_size);
    if (!escaped) { free(text); return; }
    json_escape_string(text, pos, escaped, esc_size);
    free(text);

    /* Build the params */
    int params_size = (int)strlen(escaped) + 4096;
    char *params = malloc(params_size);
    if (!params) { free(escaped); return; }

    snprintf(params, params_size,
        "{\"textDocument\":{\"uri\":\"%s\",\"languageId\":\"%s\","
        "\"version\":1,\"text\":\"%s\"}}",
        uri, lang_id, escaped);
    free(escaped);

    lsp_send_notification(ed->lsp_server, "textDocument/didOpen", params);
    free(params);
}

void editor_lsp_did_change(Editor *ed)
{
    if (!ed->lsp_server || !ed->lsp_server->initialized) return;

    Buffer *buf = editor_current_buffer(ed);
    if (!buf || !buf->filename) return;

    char uri[2048];
    lsp_path_to_uri(buf->filename, uri, sizeof(uri));

    /* Full document sync: send entire buffer contents */
    int total_len = 0;
    for (int i = 0; i < buf->num_lines; i++)
        total_len += buffer_line_len(buf, i) + 1;

    char *text = malloc(total_len + 1);
    if (!text) return;
    int pos = 0;
    for (int i = 0; i < buf->num_lines; i++) {
        const char *line = buffer_get_line(buf, i);
        int len = buffer_line_len(buf, i);
        memcpy(text + pos, line, len);
        pos += len;
        if (i < buf->num_lines - 1)
            text[pos++] = '\n';
    }
    text[pos] = '\0';

    int esc_size = pos * 4 + 1;
    char *escaped = malloc(esc_size);
    if (!escaped) { free(text); return; }
    json_escape_string(text, pos, escaped, esc_size);
    free(text);

    /* Increment a simple version counter using a static variable.
     * A proper implementation would store this per-buffer. */
    static int version = 2;

    int params_size = (int)strlen(escaped) + 4096;
    char *params = malloc(params_size);
    if (!params) { free(escaped); return; }

    snprintf(params, params_size,
        "{\"textDocument\":{\"uri\":\"%s\",\"version\":%d},"
        "\"contentChanges\":[{\"text\":\"%s\"}]}",
        uri, version++, escaped);
    free(escaped);

    lsp_send_notification(ed->lsp_server, "textDocument/didChange", params);
    free(params);
}

void editor_lsp_did_save(Editor *ed)
{
    if (!ed->lsp_server || !ed->lsp_server->initialized) return;

    Buffer *buf = editor_current_buffer(ed);
    if (!buf || !buf->filename) return;

    char uri[2048];
    lsp_path_to_uri(buf->filename, uri, sizeof(uri));

    char params[4096];
    snprintf(params, sizeof(params),
        "{\"textDocument\":{\"uri\":\"%s\"}}", uri);

    lsp_send_notification(ed->lsp_server, "textDocument/didSave", params);
}

/*
 * handle_lsp_message — process one incoming LSP message.
 *
 * Dispatches based on the "method" field (for notifications) or
 * the "id" field (for responses to our requests).
 */
static void handle_lsp_message(Editor *ed, const char *json_text)
{
    JsonValue *msg = json_parse(json_text, -1);
    if (!msg) return;

    /* Check if this is the initialize response */
    JsonValue *id_val = json_get(msg, "id");
    JsonValue *result = json_get(msg, "result");

    if (id_val && result && !ed->lsp_server->initialized) {
        /*
         * Initialize response — extract server capabilities.
         */
        ed->lsp_server->initialized = 1;

        JsonValue *caps = json_get(result, "capabilities");
        if (caps) {
            if (json_get(caps, "completionProvider"))
                ed->lsp_server->has_completion = 1;
            if (json_get_bool(json_get(caps, "definitionProvider"), 0)
                    || json_get(caps, "definitionProvider"))
                ed->lsp_server->has_definition = 1;
            if (json_get_bool(json_get(caps, "hoverProvider"), 0)
                    || json_get(caps, "hoverProvider"))
                ed->lsp_server->has_hover = 1;
            if (json_get_bool(json_get(caps, "referencesProvider"), 0)
                    || json_get(caps, "referencesProvider"))
                ed->lsp_server->has_references = 1;
            if (json_get_bool(json_get(caps, "documentFormattingProvider"), 0)
                    || json_get(caps, "documentFormattingProvider"))
                ed->lsp_server->has_formatting = 1;
            if (json_get_bool(json_get(caps, "renameProvider"), 0)
                    || json_get(caps, "renameProvider"))
                ed->lsp_server->has_rename = 1;
        }

        /* Send initialized notification (required by LSP spec) */
        lsp_send_notification(ed->lsp_server, "initialized", "{}");

        /* Now open the current document */
        editor_lsp_did_open(ed);

        editor_set_status(ed, "LSP: ready.");
        json_free(msg);
        return;
    }

    /* Check if this is a notification */
    const char *method = json_get_string(json_get(msg, "method"));
    if (method) {
        if (strcmp(method, "textDocument/publishDiagnostics") == 0) {
            /*
             * Diagnostics notification — parse and store per-buffer.
             *
             * The notification includes a URI identifying the file.
             * We find the matching buffer and update its diagnostics.
             */
            JsonValue *params = json_get(msg, "params");
            const char *uri = json_get_string(json_get(params, "uri"));

            if (uri) {
                /* Convert URI to a file path */
                char diag_path[2048];
                lsp_uri_to_path(uri, diag_path, sizeof(diag_path));

                /* Find the buffer for this file */
                for (int b = 0; b < ed->num_buffers; b++) {
                    Buffer *buf = ed->buffers[b];
                    if (!buf || !buf->filename) continue;
                    if (strcmp(buf->filename, diag_path) == 0) {
                        /* Parse diagnostics into this buffer */
                        lsp_parse_diagnostics(&buf->lsp_diagnostics, params);

                        /* If this is the current buffer, show summary */
                        if (b == ed->current_buffer) {
                            int errs = 0, warns = 0;
                            for (int d = 0; d < buf->lsp_diagnostics.count; d++) {
                                if (buf->lsp_diagnostics.items[d].severity
                                        == LSP_SEV_ERROR)
                                    errs++;
                                else
                                    warns++;
                            }
                            if (errs > 0 || warns > 0)
                                editor_set_status(ed,
                                    "LSP: %d error(s), %d warning(s)",
                                    errs, warns);
                            else
                                editor_set_status(ed, "LSP: no issues.");
                        }
                        break;
                    }
                }
            }
        }
        /* Other notifications: window/logMessage, etc. — ignore for now */
    }

    json_free(msg);
}

/*
 * lsp_wait_for_response — block until a response with the given ID arrives.
 *
 * Reads from the LSP pipe in a tight loop with a timeout.  Any notifications
 * received while waiting are dispatched normally (e.g. diagnostics).
 *
 * Returns the parsed JSON response (caller must json_free), or NULL on timeout.
 * Timeout is in milliseconds.
 */
static JsonValue *lsp_wait_for_response(Editor *ed, int request_id, int timeout_ms)
{
    if (!ed->lsp_server) return NULL;

    int elapsed = 0;
    int step = 10;  /* check every 10ms */

    while (elapsed < timeout_ms) {
        char *messages[16];
        int count = lsp_read_messages(ed->lsp_server, messages, 16);

        for (int i = 0; i < count; i++) {
            JsonValue *msg = json_parse(messages[i], -1);
            free(messages[i]);

            if (!msg) continue;

            /* Check if this is our response */
            JsonValue *id_val = json_get(msg, "id");
            if (id_val && json_get_int(id_val, -1) == request_id) {
                /* Process any remaining messages as notifications */
                for (int j = i + 1; j < count; j++) {
                    handle_lsp_message(ed, messages[j]);
                    free(messages[j]);
                }
                return msg;  /* caller frees */
            }

            /* Not our response — dispatch as notification */
            const char *method = json_get_string(json_get(msg, "method"));
            if (method) {
                /* Re-serialize isn't ideal, but handle_lsp_message
                 * needs the raw string. We can call the handler directly
                 * with the parsed message's data. For now, just handle
                 * diagnostics inline. */
                if (strcmp(method, "textDocument/publishDiagnostics") == 0) {
                    JsonValue *params = json_get(msg, "params");
                    const char *uri = json_get_string(json_get(params, "uri"));
                    if (uri) {
                        char dpath[2048];
                        lsp_uri_to_path(uri, dpath, sizeof(dpath));
                        for (int b = 0; b < ed->num_buffers; b++) {
                            Buffer *buf = ed->buffers[b];
                            if (buf && buf->filename
                                    && strcmp(buf->filename, dpath) == 0) {
                                lsp_parse_diagnostics(&buf->lsp_diagnostics,
                                                      params);
                                break;
                            }
                        }
                    }
                }
            }
            json_free(msg);
        }

        if (count == 0) {
            usleep(step * 1000);
            elapsed += step;
        }
    }

    return NULL;  /* timed out */
}

void editor_lsp_complete(Editor *ed)
{
    if (!ed->lsp_server || !ed->lsp_server->initialized) {
        editor_set_status(ed, "LSP: no server running.");
        return;
    }
    if (!ed->lsp_server->has_completion) {
        editor_set_status(ed, "LSP: server doesn't support completion.");
        return;
    }

    Buffer *buf = editor_current_buffer(ed);
    if (!buf || !buf->filename) return;

    /* Build the completion request params */
    char uri[2048];
    lsp_path_to_uri(buf->filename, uri, sizeof(uri));

    char params[4096];
    snprintf(params, sizeof(params),
        "{\"textDocument\":{\"uri\":\"%s\"},"
        "\"position\":{\"line\":%d,\"character\":%d}}",
        uri, ed->cursor_row, ed->cursor_col);

    editor_set_status(ed, "LSP: completing...");
    display_render(ed);

    /* Send request and wait for response */
    int req_id = lsp_send_request(ed->lsp_server,
                                  "textDocument/completion", params);
    if (req_id < 0) {
        editor_set_status(ed, "LSP: failed to send completion request.");
        return;
    }

    JsonValue *response = lsp_wait_for_response(ed, req_id, 3000);
    if (!response) {
        editor_set_status(ed, "LSP: completion timed out.");
        return;
    }

    /* Parse completion items */
    JsonValue *result = json_get(response, "result");
    LspCompletionItem *items = malloc(LSP_MAX_COMPLETIONS
                                      * sizeof(LspCompletionItem));
    if (!items) { json_free(response); return; }

    int num_items = lsp_parse_completions(items, LSP_MAX_COMPLETIONS, result);
    json_free(response);

    if (num_items == 0) {
        free(items);
        editor_set_status(ed, "LSP: no completions.");
        return;
    }

    /*
     * Build FinderFile entries from completion items so we can reuse
     * the finder popup UI.
     */
    FinderFile *files = malloc(num_items * sizeof(FinderFile));
    if (!files) { free(items); return; }

    for (int i = 0; i < num_items; i++) {
        if (items[i].detail[0])
            snprintf(files[i].display, sizeof(files[i].display),
                     "%s  %s", items[i].label, items[i].detail);
        else
            strncpy(files[i].display, items[i].label,
                    sizeof(files[i].display) - 1);
        files[i].display[sizeof(files[i].display) - 1] = '\0';

        /* Store the insert text in the path field */
        strncpy(files[i].path, items[i].insert_text,
                sizeof(files[i].path) - 1);
        files[i].path[sizeof(files[i].path) - 1] = '\0';
    }

    free(items);

    /* Show the popup */
    char *selected = display_finder_popup(ed, files, num_items);
    free(files);

    if (selected) {
        /*
         * Insert the completion text at the cursor.
         *
         * First, we need to remove the partial word the user already typed.
         * Walk backward from the cursor to find the start of the word.
         */
        const char *line = buffer_get_line(buf, ed->cursor_row);
        int word_start = ed->cursor_col;
        while (word_start > 0 && (line[word_start - 1] == '_'
               || (line[word_start - 1] >= 'a' && line[word_start - 1] <= 'z')
               || (line[word_start - 1] >= 'A' && line[word_start - 1] <= 'Z')
               || (line[word_start - 1] >= '0' && line[word_start - 1] <= '9')))
            word_start--;

        /* Delete the partial word */
        for (int i = ed->cursor_col - 1; i >= word_start; i--)
            buffer_delete_char(buf, ed->cursor_row, i);
        ed->cursor_col = word_start;

        /* Insert the completion text */
        for (int i = 0; selected[i]; i++) {
            buffer_insert_char(buf, ed->cursor_row, ed->cursor_col, selected[i]);
            ed->cursor_col++;
        }

        free(selected);
        editor_scroll(ed);

        /* Notify LSP of the change */
        editor_lsp_did_change(ed);
    }

    display_render(ed);
}

void editor_lsp_goto_definition(Editor *ed)
{
    if (!ed->lsp_server || !ed->lsp_server->initialized) {
        editor_set_status(ed, "LSP: no server running.");
        return;
    }
    if (!ed->lsp_server->has_definition) {
        editor_set_status(ed, "LSP: server doesn't support go-to-definition.");
        return;
    }

    Buffer *buf = editor_current_buffer(ed);
    if (!buf || !buf->filename) return;

    char uri[2048];
    lsp_path_to_uri(buf->filename, uri, sizeof(uri));

    char params[4096];
    snprintf(params, sizeof(params),
        "{\"textDocument\":{\"uri\":\"%s\"},"
        "\"position\":{\"line\":%d,\"character\":%d}}",
        uri, ed->cursor_row, ed->cursor_col);

    editor_set_status(ed, "LSP: finding definition...");
    display_render(ed);

    int req_id = lsp_send_request(ed->lsp_server,
                                  "textDocument/definition", params);
    if (req_id < 0) return;

    JsonValue *response = lsp_wait_for_response(ed, req_id, 3000);
    if (!response) {
        editor_set_status(ed, "LSP: definition request timed out.");
        return;
    }

    /*
     * The result can be:
     *   - A single Location object: {uri, range}
     *   - An array of Location objects
     *   - null (no definition found)
     */
    JsonValue *result = json_get(response, "result");
    if (!result || result->type == JSON_NULL) {
        json_free(response);
        editor_set_status(ed, "LSP: no definition found.");
        return;
    }

    /* Get the first location */
    JsonValue *loc = result;
    if (result->type == JSON_ARRAY) {
        if (json_array_len(result) == 0) {
            json_free(response);
            editor_set_status(ed, "LSP: no definition found.");
            return;
        }
        loc = json_array_get(result, 0);
    }

    const char *target_uri = json_get_string(json_get(loc, "uri"));
    JsonValue *range = json_get(loc, "range");
    JsonValue *start = json_get(range, "start");
    int target_line = json_get_int(json_get(start, "line"), 0);
    int target_col  = json_get_int(json_get(start, "character"), 0);

    if (target_uri) {
        char target_path[2048];
        lsp_uri_to_path(target_uri, target_path, sizeof(target_path));

        editor_open_or_switch(ed, target_path);

        Buffer *target_buf = editor_current_buffer(ed);
        ed->cursor_row = target_line;
        if (target_buf && ed->cursor_row >= target_buf->num_lines)
            ed->cursor_row = target_buf->num_lines - 1;
        ed->cursor_col = target_col;
        ed->desired_col = target_col;
        editor_scroll(ed);

        editor_set_status(ed, "Definition: %s:%d",
                          target_path, target_line + 1);
    }

    json_free(response);
}

void editor_lsp_hover(Editor *ed)
{
    if (!ed->lsp_server || !ed->lsp_server->initialized) {
        editor_set_status(ed, "LSP: no server running.");
        return;
    }
    if (!ed->lsp_server->has_hover) {
        editor_set_status(ed, "LSP: server doesn't support hover.");
        return;
    }

    Buffer *buf = editor_current_buffer(ed);
    if (!buf || !buf->filename) return;

    char uri[2048];
    lsp_path_to_uri(buf->filename, uri, sizeof(uri));

    char params[4096];
    snprintf(params, sizeof(params),
        "{\"textDocument\":{\"uri\":\"%s\"},"
        "\"position\":{\"line\":%d,\"character\":%d}}",
        uri, ed->cursor_row, ed->cursor_col);

    int req_id = lsp_send_request(ed->lsp_server, "textDocument/hover", params);
    if (req_id < 0) return;

    JsonValue *response = lsp_wait_for_response(ed, req_id, 3000);
    if (!response) {
        editor_set_status(ed, "LSP: hover timed out.");
        return;
    }

    JsonValue *result = json_get(response, "result");
    if (!result || result->type == JSON_NULL) {
        json_free(response);
        editor_set_status(ed, "No documentation available.");
        return;
    }

    /*
     * The hover result has a "contents" field which can be:
     *   - A string (plain text)
     *   - A MarkupContent object: {kind: "markdown"|"plaintext", value: "..."}
     *   - An array of strings/objects (deprecated but some servers use it)
     */
    const char *hover_text = NULL;
    JsonValue *contents = json_get(result, "contents");
    if (contents) {
        if (contents->type == JSON_STRING) {
            hover_text = json_get_string(contents);
        } else if (contents->type == JSON_OBJECT) {
            hover_text = json_get_string(json_get(contents, "value"));
        } else if (contents->type == JSON_ARRAY && json_array_len(contents) > 0) {
            JsonValue *first = json_array_get(contents, 0);
            if (first->type == JSON_STRING)
                hover_text = json_get_string(first);
            else if (first->type == JSON_OBJECT)
                hover_text = json_get_string(json_get(first, "value"));
        }
    }

    if (hover_text && hover_text[0]) {
        /*
         * Show the hover text in a popup.  For simplicity, we use the
         * finder popup with the text split into lines.
         */
        /* Count lines in hover text */
        int num_lines = 1;
        for (const char *p = hover_text; *p; p++)
            if (*p == '\n') num_lines++;

        FinderFile *files = malloc(num_lines * sizeof(FinderFile));
        if (files) {
            int idx = 0;
            const char *line_start = hover_text;
            for (const char *p = hover_text; ; p++) {
                if (*p == '\n' || *p == '\0') {
                    int len = (int)(p - line_start);
                    if (len >= (int)sizeof(files[idx].display))
                        len = (int)sizeof(files[idx].display) - 1;
                    memcpy(files[idx].display, line_start, len);
                    files[idx].display[len] = '\0';
                    files[idx].path[0] = '\0';  /* no action on select */
                    idx++;
                    if (*p == '\0') break;
                    line_start = p + 1;
                }
            }

            display_finder_popup(ed, files, idx);
            free(files);
        }
    } else {
        editor_set_status(ed, "No documentation available.");
    }

    json_free(response);
    display_render(ed);
}

void editor_lsp_references(Editor *ed)
{
    if (!ed->lsp_server || !ed->lsp_server->initialized) {
        editor_set_status(ed, "LSP: no server running.");
        return;
    }
    if (!ed->lsp_server->has_references) {
        editor_set_status(ed, "LSP: server doesn't support references.");
        return;
    }

    Buffer *buf = editor_current_buffer(ed);
    if (!buf || !buf->filename) return;

    char uri[2048];
    lsp_path_to_uri(buf->filename, uri, sizeof(uri));

    char params[4096];
    snprintf(params, sizeof(params),
        "{\"textDocument\":{\"uri\":\"%s\"},"
        "\"position\":{\"line\":%d,\"character\":%d},"
        "\"context\":{\"includeDeclaration\":true}}",
        uri, ed->cursor_row, ed->cursor_col);

    editor_set_status(ed, "LSP: finding references...");
    display_render(ed);

    int req_id = lsp_send_request(ed->lsp_server,
                                  "textDocument/references", params);
    if (req_id < 0) return;

    JsonValue *response = lsp_wait_for_response(ed, req_id, 5000);
    if (!response) {
        editor_set_status(ed, "LSP: references timed out.");
        return;
    }

    JsonValue *result = json_get(response, "result");
    if (!result || result->type != JSON_ARRAY || json_array_len(result) == 0) {
        json_free(response);
        editor_set_status(ed, "No references found.");
        return;
    }

    int count = json_array_len(result);
    FinderFile *files = malloc(count * sizeof(FinderFile));
    if (!files) { json_free(response); return; }

    for (int i = 0; i < count; i++) {
        JsonValue *loc = json_array_get(result, i);
        const char *loc_uri = json_get_string(json_get(loc, "uri"));
        JsonValue *range = json_get(loc, "range");
        JsonValue *start = json_get(range, "start");
        int line = json_get_int(json_get(start, "line"), 0);

        char path[2048];
        if (loc_uri)
            lsp_uri_to_path(loc_uri, path, sizeof(path));
        else
            path[0] = '\0';

        /* Display: "filename:line" */
        const char *basename = strrchr(path, '/');
        basename = basename ? basename + 1 : path;
        snprintf(files[i].display, sizeof(files[i].display),
                 "%s:%d", basename, line + 1);
        snprintf(files[i].path, sizeof(files[i].path),
                 "%s:%d", path, line + 1);
    }

    json_free(response);

    char *selected = display_finder_popup(ed, files, count);
    free(files);

    if (selected) {
        /* Parse "filepath:line" */
        char *colon = strrchr(selected, ':');
        if (colon) {
            *colon = '\0';
            int target_line = atoi(colon + 1);
            editor_open_or_switch(ed, selected);
            Buffer *tbuf = editor_current_buffer(ed);
            ed->cursor_row = target_line - 1;
            if (tbuf && ed->cursor_row >= tbuf->num_lines)
                ed->cursor_row = tbuf->num_lines - 1;
            if (ed->cursor_row < 0) ed->cursor_row = 0;
            ed->cursor_col = 0;
            ed->desired_col = 0;
            editor_scroll(ed);
        }
        free(selected);
    }

    display_render(ed);
}

void editor_lsp_format(Editor *ed)
{
    if (!ed->lsp_server || !ed->lsp_server->initialized) {
        editor_set_status(ed, "LSP: no server running.");
        return;
    }
    if (!ed->lsp_server->has_formatting) {
        editor_set_status(ed, "LSP: server doesn't support formatting.");
        return;
    }

    Buffer *buf = editor_current_buffer(ed);
    if (!buf || !buf->filename) return;

    char uri[2048];
    lsp_path_to_uri(buf->filename, uri, sizeof(uri));

    char params[4096];
    snprintf(params, sizeof(params),
        "{\"textDocument\":{\"uri\":\"%s\"},"
        "\"options\":{\"tabSize\":%d,\"insertSpaces\":true}}",
        uri, ed->tab_width);

    editor_set_status(ed, "LSP: formatting...");
    display_render(ed);

    int req_id = lsp_send_request(ed->lsp_server,
                                  "textDocument/formatting", params);
    if (req_id < 0) return;

    JsonValue *response = lsp_wait_for_response(ed, req_id, 5000);
    if (!response) {
        editor_set_status(ed, "LSP: formatting timed out.");
        return;
    }

    JsonValue *result = json_get(response, "result");
    if (!result || result->type != JSON_ARRAY || json_array_len(result) == 0) {
        json_free(response);
        editor_set_status(ed, "No formatting changes.");
        return;
    }

    /*
     * Apply text edits in REVERSE order (bottom to top) so line numbers
     * don't shift as we apply each edit.
     *
     * Each TextEdit has:
     *   range: {start: {line, character}, end: {line, character}}
     *   newText: string
     *
     * For simplicity with full-document formatting, many servers return
     * a single edit replacing the entire file.  We handle both cases.
     */
    int num_edits = json_array_len(result);
    int saved_row = ed->cursor_row;
    int saved_col = ed->cursor_col;

    /* Apply edits in reverse order */
    for (int i = num_edits - 1; i >= 0; i--) {
        JsonValue *edit = json_array_get(result, i);
        JsonValue *range = json_get(edit, "range");
        JsonValue *start = json_get(range, "start");
        JsonValue *end_pos = json_get(range, "end");
        const char *new_text = json_get_string(json_get(edit, "newText"));

        if (!new_text) continue;

        int start_line = json_get_int(json_get(start, "line"), 0);
        int start_col  = json_get_int(json_get(start, "character"), 0);
        int end_line   = json_get_int(json_get(end_pos, "line"), 0);
        int end_col    = json_get_int(json_get(end_pos, "character"), 0);

        /* Delete the range */
        if (start_line < buf->num_lines && end_line < buf->num_lines)
            buffer_delete_region(buf, start_line, start_col,
                                end_line, end_col);

        /* Insert the new text */
        int ins_row = start_line;
        int ins_col = start_col;
        for (int c = 0; new_text[c]; c++) {
            if (new_text[c] == '\n') {
                buffer_insert_newline(buf, ins_row, ins_col);
                ins_row++;
                ins_col = 0;
            } else {
                buffer_insert_char(buf, ins_row, ins_col, new_text[c]);
                ins_col++;
            }
        }
    }

    /* Restore cursor as close to original position as possible */
    ed->cursor_row = saved_row;
    if (ed->cursor_row >= buf->num_lines)
        ed->cursor_row = buf->num_lines - 1;
    ed->cursor_col = saved_col;
    ed->desired_col = saved_col;
    editor_scroll(ed);

    json_free(response);

    /* Notify LSP of the change */
    editor_lsp_did_change(ed);
    editor_set_status(ed, "Formatted.");
}

void editor_lsp_rename(Editor *ed)
{
    if (!ed->lsp_server || !ed->lsp_server->initialized) {
        editor_set_status(ed, "LSP: no server running.");
        return;
    }
    if (!ed->lsp_server->has_rename) {
        editor_set_status(ed, "LSP: server doesn't support rename.");
        return;
    }

    Buffer *buf = editor_current_buffer(ed);
    if (!buf || !buf->filename) return;

    /* Prompt for the new name */
    char *new_name = display_prompt(ed, "Rename to: ");
    if (!new_name || new_name[0] == '\0') {
        free(new_name);
        editor_set_status(ed, "Rename cancelled.");
        return;
    }

    char uri[2048];
    lsp_path_to_uri(buf->filename, uri, sizeof(uri));

    /* Escape the new name for JSON */
    char escaped_name[512];
    json_escape_string(new_name, -1, escaped_name, sizeof(escaped_name));
    free(new_name);

    char params[4096];
    snprintf(params, sizeof(params),
        "{\"textDocument\":{\"uri\":\"%s\"},"
        "\"position\":{\"line\":%d,\"character\":%d},"
        "\"newName\":\"%s\"}",
        uri, ed->cursor_row, ed->cursor_col, escaped_name);

    editor_set_status(ed, "LSP: renaming...");
    display_render(ed);

    int req_id = lsp_send_request(ed->lsp_server,
                                  "textDocument/rename", params);
    if (req_id < 0) return;

    JsonValue *response = lsp_wait_for_response(ed, req_id, 5000);
    if (!response) {
        editor_set_status(ed, "LSP: rename timed out.");
        return;
    }

    JsonValue *result = json_get(response, "result");
    if (!result || result->type == JSON_NULL) {
        json_free(response);
        editor_set_status(ed, "LSP: rename failed (no result).");
        return;
    }

    /*
     * The result is a WorkspaceEdit with a "changes" object:
     *   { "file:///path": [TextEdit, ...], ... }
     *
     * For each file, apply the edits.  We only handle files that are
     * currently open — edits to other files are skipped with a warning.
     */
    JsonValue *changes = json_get(result, "changes");
    int total_edits = 0;

    if (changes && changes->type == JSON_OBJECT) {
        for (int f = 0; f < changes->object.count; f++) {
            const char *file_uri = changes->object.keys[f];
            JsonValue *edits = changes->object.vals[f];

            char file_path[2048];
            lsp_uri_to_path(file_uri, file_path, sizeof(file_path));

            /* Find the buffer for this file */
            editor_open_or_switch(ed, file_path);
            Buffer *target = editor_current_buffer(ed);
            if (!target) continue;

            /* Apply edits in reverse order */
            int num_edits = json_array_len(edits);
            for (int i = num_edits - 1; i >= 0; i--) {
                JsonValue *edit = json_array_get(edits, i);
                JsonValue *range = json_get(edit, "range");
                JsonValue *start = json_get(range, "start");
                JsonValue *end_pos = json_get(range, "end");
                const char *new_text = json_get_string(
                    json_get(edit, "newText"));
                if (!new_text) continue;

                int sl = json_get_int(json_get(start, "line"), 0);
                int sc = json_get_int(json_get(start, "character"), 0);
                int el = json_get_int(json_get(end_pos, "line"), 0);
                int ec = json_get_int(json_get(end_pos, "character"), 0);

                if (sl < target->num_lines && el < target->num_lines)
                    buffer_delete_region(target, sl, sc, el, ec);

                int ir = sl, ic = sc;
                for (int c = 0; new_text[c]; c++) {
                    if (new_text[c] == '\n') {
                        buffer_insert_newline(target, ir, ic);
                        ir++; ic = 0;
                    } else {
                        buffer_insert_char(target, ir, ic, new_text[c]);
                        ic++;
                    }
                }
                total_edits++;
            }

            editor_lsp_did_change(ed);
        }
    }

    json_free(response);
    editor_scroll(ed);
    editor_set_status(ed, "Renamed: %d edit(s) applied.", total_edits);
}

void editor_lsp_signature_help(Editor *ed)
{
    if (!ed->lsp_server || !ed->lsp_server->initialized) {
        editor_set_status(ed, "LSP: no server running.");
        return;
    }

    Buffer *buf = editor_current_buffer(ed);
    if (!buf || !buf->filename) return;

    char uri[2048];
    lsp_path_to_uri(buf->filename, uri, sizeof(uri));

    char params[4096];
    snprintf(params, sizeof(params),
        "{\"textDocument\":{\"uri\":\"%s\"},"
        "\"position\":{\"line\":%d,\"character\":%d}}",
        uri, ed->cursor_row, ed->cursor_col);

    int req_id = lsp_send_request(ed->lsp_server,
                                  "textDocument/signatureHelp", params);
    if (req_id < 0) return;

    JsonValue *response = lsp_wait_for_response(ed, req_id, 2000);
    if (!response) {
        editor_set_status(ed, "LSP: signature help timed out.");
        return;
    }

    JsonValue *result = json_get(response, "result");
    if (!result || result->type == JSON_NULL) {
        json_free(response);
        editor_set_status(ed, "No signature info available.");
        return;
    }

    /*
     * SignatureHelp has:
     *   signatures: array of SignatureInformation
     *   activeSignature: number
     *   activeParameter: number
     *
     * Each SignatureInformation has:
     *   label: string (e.g. "printf(const char *format, ...)")
     *   parameters: array of ParameterInformation
     */
    JsonValue *sigs = json_get(result, "signatures");
    int active_sig = json_get_int(json_get(result, "activeSignature"), 0);

    if (!sigs || json_array_len(sigs) == 0) {
        json_free(response);
        editor_set_status(ed, "No signature info available.");
        return;
    }

    JsonValue *sig = json_array_get(sigs, active_sig);
    if (!sig) sig = json_array_get(sigs, 0);

    const char *label = json_get_string(json_get(sig, "label"));
    if (label && label[0])
        editor_set_status(ed, "%s", label);
    else
        editor_set_status(ed, "No signature info available.");

    json_free(response);
}

void editor_lsp_poll(Editor *ed)
{
    if (!ed->lsp_server) return;

    /*
     * Read any available messages from the server (non-blocking).
     * lsp_read_messages returns 0 if no data is available.
     */
    char *messages[16];
    int count = lsp_read_messages(ed->lsp_server, messages, 16);

    for (int i = 0; i < count; i++) {
        handle_lsp_message(ed, messages[i]);
        free(messages[i]);
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
