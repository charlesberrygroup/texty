/*
 * buffer.h — Text Buffer
 * =============================================================================
 * A Buffer holds the in-memory representation of a single open file.
 *
 * DESIGN: Array-of-Lines
 * ----------------------
 * Text is stored as a dynamic array of Line structs. Each Line owns a
 * heap-allocated, null-terminated char array for its text content.
 *
 * This design is easy to reason about:
 *   - Insert a character  → grow one Line's text array and shift bytes.
 *   - Press Enter         → split one Line into two.
 *   - Press Backspace     → either shrink a Line, or join two Lines.
 *
 * Trade-off: inserting/deleting in the middle of a very large line is O(n)
 * in the line length. For Phase 1 this is fine. A "gap buffer" or "piece
 * table" would be faster but is significantly more complex to implement.
 * =============================================================================
 */

#ifndef BUFFER_H
#define BUFFER_H

#include "undo.h"   /* UndoStack — each buffer tracks its own edit history */

/* ---- Constants ------------------------------------------------------------ */

/** Starting capacity (in bytes) for a freshly created Line's text array. */
#define LINE_INITIAL_CAP     64

/** Starting capacity (number of Line slots) for a new Buffer. */
#define BUFFER_INITIAL_LINES 64

/* ---- Data Types ----------------------------------------------------------- */

/**
 * Line — a single line of text within a buffer.
 *
 *   text  Heap-allocated string.  Always null-terminated ('\0' at text[len]).
 *   len   Number of visible characters (does NOT include the null terminator).
 *   cap   Number of bytes allocated for text (always > len).
 */
typedef struct {
    char *text;
    int   len;
    int   cap;
} Line;

/**
 * Buffer — the in-memory contents of a file being edited.
 *
 *   lines      Dynamic array of Line structs.
 *   num_lines  How many lines are currently in the buffer (always >= 1).
 *   capacity   How many Line slots are allocated in the array.
 *   filename   Heap-allocated path to the file on disk.  NULL for new files.
 *   dirty      Non-zero when there are unsaved changes.
 *   undo_stack Stack of past operations — pop to undo.
 *   redo_stack Stack of undone operations — pop to redo.
 *              Cleared whenever a new edit is made.
 */
typedef struct {
    Line      *lines;
    int        num_lines;
    int        capacity;
    char      *filename;
    int        dirty;
    UndoStack  undo_stack;
    UndoStack  redo_stack;
} Buffer;

/* ---- Lifecycle ------------------------------------------------------------ */

/**
 * buffer_create — allocate and return a new, empty Buffer.
 *
 * The new buffer has one empty line and no filename.  Returns NULL on
 * allocation failure.
 */
Buffer *buffer_create(void);

/**
 * buffer_destroy — free all memory owned by buf, including buf itself.
 *
 * After calling this, the pointer is invalid and must not be used.
 */
void buffer_destroy(Buffer *buf);

/* ---- File I/O ------------------------------------------------------------- */

/**
 * buffer_load_file — load the file at `path` into buf.
 *
 * Replaces any existing content.  On success the buffer's filename is set
 * and dirty is cleared.  Returns 0 on success, -1 on error (check errno).
 */
int buffer_load_file(Buffer *buf, const char *path);

/**
 * buffer_save_file — write buf's content to buf->filename.
 *
 * Returns 0 on success, -1 on error.  If buf->filename is NULL, returns -1
 * (use buffer_save_file_as instead).
 */
int buffer_save_file(Buffer *buf);

/**
 * buffer_save_file_as — write buf's content to `path`, updating the filename.
 *
 * Returns 0 on success, -1 on error.
 */
int buffer_save_file_as(Buffer *buf, const char *path);

/* ---- Text Mutation -------------------------------------------------------- */

/**
 * buffer_insert_char — insert character `c` at position (row, col).
 *
 *   row   Line index, 0-based.  Must be in [0, num_lines).
 *   col   Column index, 0-based.  Must be in [0, line.len].
 *         Inserting at col=line.len appends to the end of the line.
 *
 * Returns 0 on success, -1 if out of bounds or allocation fails.
 * Marks the buffer dirty on success.
 */
int buffer_insert_char(Buffer *buf, int row, int col, char c);

/**
 * buffer_delete_char — delete the character at position (row, col).
 *
 *   col must be in [0, line.len).  (You cannot delete past the end of a line.)
 *
 * Returns 0 on success, -1 if out of bounds.
 * Marks the buffer dirty on success.
 */
int buffer_delete_char(Buffer *buf, int row, int col);

/**
 * buffer_insert_newline — split line `row` at column `col`.
 *
 * Text from col to end-of-line moves to a new line inserted at row+1.
 * The portion before col stays on row.
 *
 * Returns 0 on success, -1 on error.
 * Marks the buffer dirty on success.
 */
int buffer_insert_newline(Buffer *buf, int row, int col);

/**
 * buffer_join_lines — join line `row` with line `row+1`.
 *
 * The content of row+1 is appended to row, and row+1 is removed.
 * This is what happens when Backspace is pressed at column 0.
 *
 * Returns 0 on success, -1 if row+1 does not exist or on allocation failure.
 * Marks the buffer dirty on success.
 */
int buffer_join_lines(Buffer *buf, int row);

/* ---- Queries -------------------------------------------------------------- */

/**
 * buffer_get_line — return the text of line `row`.
 *
 * Returns a read-only pointer to the internal string.  Do not modify or free.
 * Returns NULL if row is out of bounds.
 */
const char *buffer_get_line(const Buffer *buf, int row);

/**
 * buffer_line_len — return the length (number of chars) of line `row`.
 *
 * Returns -1 if row is out of bounds.
 */
int buffer_line_len(const Buffer *buf, int row);

#endif /* BUFFER_H */
