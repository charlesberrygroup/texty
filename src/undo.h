/*
 * undo.h — Undo / Redo History
 * =============================================================================
 * This module defines the data structures that record editing history so that
 * the user can undo (Ctrl+Z) and redo (Ctrl+Y) changes.
 *
 * HOW UNDO/REDO WORKS
 * -------------------
 * Every time the user makes an edit, we record what happened in an UndoRecord
 * and push it onto the buffer's undo stack.  To undo, we pop the record off the
 * undo stack, reverse the operation, and push the record onto the redo stack.
 * To redo, we pop from the redo stack, re-apply the operation, and push back
 * onto the undo stack.  Making any new edit clears the redo stack.
 *
 * There are exactly four primitive operations that change the text:
 *   1. Insert a character         (insert_char)
 *   2. Delete a character         (delete_char)
 *   3. Insert a newline / split   (insert_newline)
 *   4. Join two lines             (join_lines)
 *
 * Each operation has a clear inverse:
 *   insert_char   ←→  delete_char
 *   insert_newline ←→ join_lines
 *
 * STACK LIMIT
 * -----------
 * Each stack holds up to UNDO_MAX records.  When full, the oldest record is
 * silently discarded to make room for the new one.
 * =============================================================================
 */

#ifndef UNDO_H
#define UNDO_H

/* Maximum number of undo/redo steps stored per buffer. */
#define UNDO_MAX 1000

/* ---- UndoType -------------------------------------------------------------- */

/*
 * UndoType — which of the four primitive operations this record describes.
 *
 * "typedef enum" is a C idiom: it defines both the enum and a convenient
 * type alias so we can write "UndoType" instead of "enum UndoType".
 */
typedef enum {
    UNDO_INSERT_CHAR,     /* a single character was inserted   */
    UNDO_DELETE_CHAR,     /* a single character was deleted    */
    UNDO_INSERT_NEWLINE,  /* Enter was pressed, line was split */
    UNDO_JOIN_LINES,      /* two lines were merged into one    */
} UndoType;

/* ---- UndoRecord ------------------------------------------------------------ */

/*
 * UndoRecord — everything needed to undo OR redo one editing operation.
 *
 * Fields
 * ------
 *   type               Which operation this record describes.
 *   row, col           Where in the buffer the operation happened.
 *   c                  The character involved (only for INSERT_CHAR / DELETE_CHAR).
 *
 *   cursor_row_before  Cursor position just BEFORE the operation.
 *   cursor_col_before  Used when undoing — we restore the cursor here.
 *
 *   cursor_row_after   Cursor position just AFTER the operation.
 *   cursor_col_after   Used when redoing — we restore the cursor here.
 *
 * Why store both before and after?
 * Because the same record is used for both undo and redo.  When undoing,
 * we reverse the operation and snap the cursor to `_before`.  When redoing,
 * we re-apply the operation and snap the cursor to `_after`.
 */
typedef struct {
    UndoType type;
    int      row;
    int      col;
    char     c;                  /* only meaningful for INSERT_CHAR / DELETE_CHAR */

    int      cursor_row_before;
    int      cursor_col_before;
    int      cursor_row_after;
    int      cursor_col_after;
} UndoRecord;

/* ---- UndoStack ------------------------------------------------------------- */

/*
 * UndoStack — a fixed-size stack of UndoRecords.
 *
 * Implemented as a simple array.  `top` is the index of the next empty slot.
 * When top == 0, the stack is empty.
 * When top == UNDO_MAX, the stack is full.
 *
 * C note: declaring a fixed-size array inside a struct means the array lives
 * right inside the struct — no separate heap allocation is needed.
 */
typedef struct {
    UndoRecord records[UNDO_MAX]; /* storage for all records          */
    int        top;               /* next empty slot (0 = empty)      */
} UndoStack;

/* ---- Functions ------------------------------------------------------------- */

/*
 * undo_clear — empty the stack (does not free anything; arrays are in-struct).
 */
void undo_clear(UndoStack *s);

/*
 * undo_is_empty — return 1 if the stack has no records, 0 otherwise.
 */
int undo_is_empty(const UndoStack *s);

/*
 * undo_push — push a record onto the stack.
 *
 * If the stack is full, the oldest record is silently dropped to make room.
 */
void undo_push(UndoStack *s, UndoRecord rec);

/*
 * undo_pop — pop the top record off the stack and store it in *out.
 *
 * Returns 1 on success, 0 if the stack was empty (in which case *out is
 * not written).
 */
int undo_pop(UndoStack *s, UndoRecord *out);

#endif /* UNDO_H */
