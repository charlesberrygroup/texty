/*
 * editor.h — Editor State
 * =============================================================================
 * The Editor struct is the central "god object" of the program, but rather
 * than containing all logic it contains only state.  All heavy operations
 * are in their own modules (buffer, display, input).
 *
 * The Editor coordinates:
 *   - Which buffers (files) are open
 *   - Where the cursor is
 *   - Which portion of the file is visible on screen (the viewport)
 *   - The current terminal dimensions
 *   - A one-line status message for the status bar
 * =============================================================================
 */

#ifndef EDITOR_H
#define EDITOR_H

#include "buffer.h"
#include "pane.h"
#include <stdarg.h>

/*
 * Forward declaration of FileTree.
 *
 * C requires that a type be declared before it can be used.  Because
 * filetree.h includes editor.h (indirectly via other headers) we would get
 * circular includes if we #include "filetree.h" here.  Instead, we tell the
 * compiler "trust me, struct FileTree exists somewhere" — enough information
 * for the compiler to handle a POINTER to it (struct FileTree *) without
 * needing to know its full definition.
 *
 * The full definition is in filetree.h; editor.c #includes both.
 */
struct FileTree;

/* ---- Constants ------------------------------------------------------------ */

/** Maximum number of files that can be open simultaneously. */
#define EDITOR_MAX_BUFFERS  32

/** Maximum length of the status message string. */
#define STATUS_MSG_LEN      256

/* ---- Data Types ----------------------------------------------------------- */

/**
 * Editor — the top-level application state.
 *
 * Per-viewport state (cursor, viewport, selection, region) has been moved
 * into the Pane struct (see pane.h).  The Editor owns a tree of panes and
 * a pointer to the currently active one (active_pane).
 *
 * To access the cursor:   ed->active_pane->cursor_row / cursor_col
 * To access the viewport:  ed->active_pane->view_row / view_col
 * To access the buffer:    ed->buffers[ed->active_pane->buffer_index]
 *                          or editor_current_buffer(ed)
 *
 * Terminal size
 * -------------
 *   term_rows / term_cols   Set by display_update_size() at startup and on
 *                           every terminal resize event (SIGWINCH).
 */
typedef struct Editor {
    /* Open buffers — shared across all panes */
    Buffer  *buffers[EDITOR_MAX_BUFFERS];
    int      num_buffers;

    /* ---- Pane tree --------------------------------------------------------
     *
     * The pane system replaces the old single-viewport model.  All per-viewport
     * state (cursor, viewport, selection, region) lives in the Pane struct
     * (see pane.h).  The Editor owns a tree of PaneNodes whose leaves are the
     * actual Pane viewports.
     *
     * active_pane always points to the pane that currently receives keyboard
     * input.  In the initial single-pane state, it's the only pane in the tree.
     *
     * To access the current cursor position:  ed->active_pane->cursor_row
     * To access the current buffer index:     ed->active_pane->buffer_index
     */
    PaneNode *pane_root;       /* root of the split-pane binary tree */
    Pane     *active_pane;     /* currently focused pane (always a leaf) */
    int       num_panes;       /* total number of leaf panes */

    /* Terminal size (updated by display_update_size) */
    int      term_rows;
    int      term_cols;

    /* Status message — shown in the status bar briefly */
    char     status_msg[STATUS_MSG_LEN];

    /* Non-zero when the editor should exit the main loop */
    int      should_quit;

    /*
     * Tab width — number of spaces inserted when the user presses Tab.
     * Defaults to 4.  Will be made configurable via config file in a later phase.
     */
    int      tab_width;

    /*
     * show_whitespace — when non-zero, spaces are rendered as a visible dot
     * (·) so the user can see indentation and trailing spaces.
     * Toggle with F2.
     */
    int      show_whitespace;

    /*
     * word_wrap — when non-zero, lines longer than the terminal width are
     * wrapped visually onto the next screen row instead of being clipped.
     * The buffer contents are not changed; this is display-only.
     * Toggle with F4.
     */
    int      word_wrap;

    /*
     * Internal clipboard — shared across all panes.
     * A heap-allocated string holding the most recently copied or cut text.
     * May contain '\n' characters for multi-line content.
     * NULL if nothing has been copied yet.
     */
    char    *clipboard;

    /*
     * Search query — shared across all panes.
     * The query string is global so all panes highlight the same matches.
     * Each pane has its own search_match_row/col (the "current match" cursor).
     *
     * Empty string means no active search (highlights are off).
     */
    char     search_query[256];

    /* ---- File explorer ---------------------------------------------------- */

    /*
     * filetree — pointer to the file explorer tree state.
     *
     * NULL means the tree has not been created yet (panel has never been opened).
     * When non-NULL, the FileTree is heap-allocated and must be freed with
     * filetree_free() in editor_cleanup().
     */
    struct FileTree *filetree;

    /** show_filetree — non-zero when the file explorer panel is visible. */
    int      show_filetree;

    /** filetree_focus — non-zero when keyboard input goes to the file tree. */
    int      filetree_focus;

    /** filetree_cursor — index into ft->entries[] of the highlighted row. */
    int      filetree_cursor;

    /** filetree_scroll — first visible entry in the file explorer. */
    int      filetree_scroll;
} Editor;

/* ---- Lifecycle ------------------------------------------------------------ */

/**
 * editor_init — zero-initialise the editor and create an empty starting buffer.
 */
void editor_init(Editor *ed);

/**
 * editor_cleanup — destroy all open buffers and free editor resources.
 */
void editor_cleanup(Editor *ed);

/* ---- Buffer management ---------------------------------------------------- */

/**
 * editor_new_buffer — create a new, empty unnamed buffer and make it current.
 *
 * Returns 0 on success, -1 if the buffer limit is reached.
 */
int editor_new_buffer(Editor *ed);

/**
 * editor_open_file — open `path` in a new buffer and make it current.
 *
 * Returns 0 on success, -1 on error (e.g. file not found).
 */
int editor_open_file(Editor *ed, const char *path);

/**
 * editor_open_or_switch — switch to an existing buffer for `path`, or open it.
 *
 * If any open buffer already has the same filename as `path`, that buffer is
 * made current (no new buffer is created).  Otherwise behaves like
 * editor_open_file().
 *
 * Returns 0 on success, -1 on error.
 */
int editor_open_or_switch(Editor *ed, const char *path);

/**
 * editor_save — save the current buffer to disk.
 *
 * Returns 0 on success, -1 on error.
 */
int editor_save(Editor *ed);

/**
 * editor_next_buffer — switch to the next open buffer (wraps around).
 */
void editor_next_buffer(Editor *ed);

/**
 * editor_prev_buffer — switch to the previous open buffer (wraps around).
 */
void editor_prev_buffer(Editor *ed);

/**
 * editor_close_buffer — close the current buffer.
 *
 * If the buffer has unsaved changes, the first call shows a warning and
 * requires a second Ctrl+W to confirm.  If this is the last buffer, a new
 * empty buffer is created so there is always at least one buffer open.
 */
void editor_close_buffer(Editor *ed);

/**
 * editor_current_buffer — return a pointer to the currently active Buffer.
 *
 * May return NULL if no buffers are open (should not happen in normal use).
 */
Buffer *editor_current_buffer(Editor *ed);

/* ---- Cursor movement ------------------------------------------------------ */

/** Move the cursor one line up.    Clamps at row 0. */
void editor_move_up(Editor *ed);

/** Move the cursor one line down.  Clamps at last line. */
void editor_move_down(Editor *ed);

/** Move the cursor one column left.  Wraps to end of previous line at col 0. */
void editor_move_left(Editor *ed);

/** Move the cursor one column right. Wraps to start of next line at end of line. */
void editor_move_right(Editor *ed);

/** Move the cursor to column 0 of the current line. */
void editor_move_line_start(Editor *ed);

/** Move the cursor to the last column of the current line. */
void editor_move_line_end(Editor *ed);

/** Move the cursor up by one screenful (Page Up). */
void editor_page_up(Editor *ed);

/** Move the cursor down by one screenful (Page Down). */
void editor_page_down(Editor *ed);

/** Move the cursor to the very first line and column. */
void editor_move_file_start(Editor *ed);

/** Move the cursor to the very last line, end of line. */
void editor_move_file_end(Editor *ed);

/* ---- Text editing --------------------------------------------------------- */

/**
 * editor_insert_char — insert `c` at the cursor position.
 *
 * Advances the cursor one column to the right.
 */
void editor_insert_char(Editor *ed, char c);

/**
 * editor_insert_pair — insert an opening character and its closing pair,
 * then place the cursor between them.
 *
 * Used for auto-close brackets and quotes.  For example, typing '(' calls
 * editor_insert_pair(ed, '(', ')'), which inserts "()" and leaves the cursor
 * at the position between them so the user can type the contents immediately.
 *
 * The entire pair is recorded as a single UNDO_PASTE entry, so one Ctrl+Z
 * removes both characters at once.
 */
void editor_insert_pair(Editor *ed, char open, char close);

/**
 * editor_find_bracket_match — find the bracket that matches the one under
 * the cursor.
 *
 * If the cursor is on '(', ')', '[', ']', '{', or '}', scans the buffer
 * to find its pair (handling nesting) and writes the result to *out_row
 * and *out_col.
 *
 * Returns 1 if a match was found, 0 if the cursor is not on a bracket or
 * no matching bracket exists in the buffer.
 */
int editor_find_bracket_match(const Editor *ed, int *out_row, int *out_col);

/**
 * editor_goto_line — prompt for a line number and jump to it (Ctrl+G).
 *
 * Line numbers are 1-based (matching what the status bar shows).
 * Out-of-range numbers are clamped to the first or last line.
 */
void editor_goto_line(Editor *ed);

/**
 * editor_toggle_word_wrap — toggle word-wrap mode (F4).
 *
 * When on, lines wider than the terminal are wrapped visually.
 * Buffer contents are never changed.
 */
void editor_toggle_word_wrap(Editor *ed);

/**
 * editor_toggle_whitespace — toggle visible whitespace rendering (F2).
 *
 * When on, spaces are drawn as '·' so indentation and trailing spaces are
 * visible.  The toggle state is stored in ed->show_whitespace.
 */
void editor_toggle_whitespace(Editor *ed);

/**
 * editor_insert_tab — insert tab_width spaces at the cursor position (Tab key).
 *
 * Recorded as a single undo entry so one Ctrl+Z removes all the spaces at once.
 */
void editor_insert_tab(Editor *ed);

/**
 * editor_insert_newline — insert a newline at the cursor position.
 *
 * Splits the current line.  Cursor moves to column 0 of the new line.
 */
void editor_insert_newline(Editor *ed);

/**
 * editor_backspace — delete the character immediately to the left of the cursor.
 *
 * At column 0, joins the current line with the one above it.
 */
void editor_backspace(Editor *ed);

/**
 * editor_delete_char — delete the character under the cursor (Delete key).
 *
 * At end of line, joins the current line with the one below it.
 */
void editor_delete_char(Editor *ed);

/* ---- Search & Replace ----------------------------------------------------- */

/**
 * editor_search_clear — clear the active search (removes all highlights).
 */
void editor_search_clear(Editor *ed);

/**
 * editor_find — prompt for a search string and jump to the first match
 * after the cursor.  Wraps around to the start of the file if needed.
 */
void editor_find(Editor *ed);

/**
 * editor_find_next — jump to the next match of the current search query.
 * Calls editor_find() if no query is set.  Wraps around.
 */
void editor_find_next(Editor *ed);

/**
 * editor_find_prev — jump to the previous match.  Wraps around.
 */
void editor_find_prev(Editor *ed);

/**
 * editor_replace — prompt for a search string and a replacement string,
 * then replace every occurrence in the current buffer.  Each replacement
 * is recorded individually in the undo history (Ctrl+Z undoes them one
 * by one in reverse order).
 */
void editor_replace(Editor *ed);

/* ---- Selection ------------------------------------------------------------ */

/**
 * editor_selection_clear — deactivate any active selection.
 *
 * Call this before any plain (non-shift) cursor movement so the selection
 * is dropped when the user moves without holding Shift.
 */
void editor_selection_clear(Editor *ed);

/**
 * editor_select_left / right / up / down — extend the selection by one unit.
 *
 * If no selection is active yet, the anchor is set at the current cursor
 * position before moving.  The cursor then moves normally and the region
 * between anchor and new cursor becomes the selection.
 */
void editor_select_left(Editor *ed);
void editor_select_right(Editor *ed);
void editor_select_up(Editor *ed);
void editor_select_down(Editor *ed);
void editor_select_line_start(Editor *ed);
void editor_select_line_end(Editor *ed);

/**
 * editor_select_all — select the entire buffer contents (Ctrl+A).
 */
void editor_select_all(Editor *ed);

/**
 * editor_copy — copy the selected text to the internal clipboard (Ctrl+C).
 *
 * Does nothing (shows status message) if no selection is active.
 * The selection is NOT cleared — the user can keep editing with it.
 */
void editor_copy(Editor *ed);

/**
 * editor_cut — copy the selected text then delete it (Ctrl+X).
 *
 * Records the deletion as a single UNDO_CUT entry so one Ctrl+Z restores it.
 * Does nothing if no selection is active.
 */
void editor_cut(Editor *ed);

/**
 * editor_paste — insert the clipboard text at the cursor (Ctrl+V).
 *
 * Records the insertion as a single UNDO_PASTE entry.
 * Does nothing if the clipboard is empty.
 */
void editor_paste(Editor *ed);

/* ---- Undo / Redo ---------------------------------------------------------- */

/**
 * editor_undo — reverse the most recent editing operation (Ctrl+Z).
 *
 * Pops one record off the current buffer's undo stack, reverses the
 * operation, and pushes the record onto the redo stack.
 * Does nothing (and shows a status message) if there is nothing to undo.
 */
void editor_undo(Editor *ed);

/**
 * editor_redo — re-apply the most recently undone operation (Ctrl+Y).
 *
 * Pops one record off the redo stack, re-applies the operation, and pushes
 * the record back onto the undo stack.
 * Does nothing (and shows a status message) if there is nothing to redo.
 */
void editor_redo(Editor *ed);

/* ---- File explorer ------------------------------------------------------- */

/**
 * editor_toggle_filetree — show or hide the file explorer panel (Ctrl+B).
 *
 * If the panel is currently hidden: shows it, gives it keyboard focus, and
 * creates (or rebuilds) the FileTree rooted at the current buffer's directory.
 *
 * If the panel is currently visible: hides it and returns focus to the editor.
 */
void editor_toggle_filetree(Editor *ed);

/* ---- Region highlight ----------------------------------------------------- */

/**
 * editor_mark_region — mark the selected rows as a persistent region (Ctrl+U).
 *
 * Behaviour:
 *   - If a selection is active: captures the selection's row span as the
 *     region, then clears the selection.  The red box border appears around
 *     those rows immediately.
 *   - If no selection but a region is active: clears the region.
 *   - If neither: shows a help message in the status bar.
 */
void editor_mark_region(Editor *ed);

/* ---- Misc ----------------------------------------------------------------- */

/**
 * editor_set_status — set a printf-style status message.
 *
 * The message is shown in the status bar.  It is NOT automatically cleared;
 * the display module may choose to clear it after a timeout (Phase 2 feature).
 */
void editor_set_status(Editor *ed, const char *fmt, ...);

/**
 * editor_scroll — adjust view_row / view_col to keep the cursor visible.
 *
 * Call this after any cursor movement or text edit operation.
 * The usable editor area height is (term_rows - 1) — one row is reserved
 * for the status bar.
 */
void editor_scroll(Editor *ed);

#endif /* EDITOR_H */
