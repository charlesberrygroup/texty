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
#include <stdarg.h>

/* ---- Constants ------------------------------------------------------------ */

/** Maximum number of files that can be open simultaneously. */
#define EDITOR_MAX_BUFFERS  32

/** Maximum length of the status message string. */
#define STATUS_MSG_LEN      256

/* ---- Data Types ----------------------------------------------------------- */

/**
 * Editor — the top-level application state.
 *
 * Cursor fields
 * -------------
 *   cursor_row     Current line (0-based).
 *   cursor_col     Current column (0-based, clamped to actual line length).
 *   desired_col    The column the user is "aiming for" during vertical movement.
 *                  When you press Down from column 20 onto a line of only 5
 *                  characters, desired_col stays at 20.  If you then move down
 *                  onto a line of 30 characters, the cursor snaps back to 20.
 *
 * Viewport fields
 * ---------------
 *   view_row / view_col   The top-left corner of the visible area.
 *                         The display renders lines [view_row .. view_row+rows)
 *                         and columns [view_col .. view_col+cols).
 *
 * Terminal size
 * -------------
 *   term_rows / term_cols   Set by display_update_size() at startup and on
 *                           every terminal resize event (SIGWINCH).
 */
typedef struct Editor {
    /* Open buffers */
    Buffer  *buffers[EDITOR_MAX_BUFFERS];
    int      num_buffers;
    int      current_buffer;    /* Index into buffers[] */

    /* Cursor */
    int      cursor_row;
    int      cursor_col;
    int      desired_col;       /* See note above */

    /* Viewport */
    int      view_row;
    int      view_col;

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
     * Selection state
     * ---------------
     * A selection is active when sel_active != 0.  It spans from the anchor
     * point to the current cursor position.  The anchor is set when the user
     * starts a Shift+Arrow movement.  The cursor moves normally; the anchor
     * stays fixed until the selection is cleared.
     *
     * Either endpoint can be the "earlier" one — display.c normalizes them.
     */
    int      sel_active;
    int      sel_anchor_row;
    int      sel_anchor_col;

    /*
     * Internal clipboard
     * ------------------
     * A heap-allocated string holding the most recently copied or cut text.
     * May contain '\n' characters for multi-line content.
     * NULL if nothing has been copied yet.
     */
    char    *clipboard;

    /*
     * Search state
     * ------------
     * search_query     The current search string.  Empty string means no
     *                  active search (highlights are off).
     *
     * search_match_row / search_match_col
     *                  Position of the match that the cursor jumped to most
     *                  recently (the one shown with the green highlight).
     *                  -1 / -1 when no match has been found yet.
     */
    char     search_query[256];
    int      search_match_row;
    int      search_match_col;
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
