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
