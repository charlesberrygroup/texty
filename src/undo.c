/*
 * undo.c — Undo / Redo Stack Implementation
 * =============================================================================
 * See undo.h for the data structures and API documentation.
 * =============================================================================
 */

#include "undo.h"

#include <stdlib.h>   /* for free */
#include <string.h>   /* for memmove */

/* ============================================================================
 * undo_clear
 * ============================================================================ */

void undo_clear(UndoStack *s)
{
    /*
     * CUT and PASTE records carry a heap-allocated `text` pointer.
     * We must free those before marking the stack empty, otherwise that
     * memory leaks every time the redo stack is cleared by a new edit.
     */
    for (int i = 0; i < s->top; i++) {
        if (s->records[i].text) {
            free(s->records[i].text);
            s->records[i].text = NULL;
        }
    }
    s->top = 0;
}

/* ============================================================================
 * undo_is_empty
 * ============================================================================ */

int undo_is_empty(const UndoStack *s)
{
    return s->top == 0;
}

/* ============================================================================
 * undo_push
 * ============================================================================ */

void undo_push(UndoStack *s, UndoRecord rec)
{
    if (s->top == UNDO_MAX) {
        /*
         * Stack is full.  Drop the oldest record (index 0) to make room.
         * Free its text pointer first if it has one, so we don't leak memory.
         */
        if (s->records[0].text) {
            free(s->records[0].text);
            s->records[0].text = NULL;
        }

        /*
         * memmove shifts every record one slot earlier, overwriting records[0]
         * and freeing up the last slot for the new record.
         */
        memmove(&s->records[0],
                &s->records[1],
                (UNDO_MAX - 1) * sizeof(UndoRecord));

        s->top--;
    }

    s->records[s->top] = rec;
    s->top++;
}

/* ============================================================================
 * undo_pop
 * ============================================================================ */

int undo_pop(UndoStack *s, UndoRecord *out)
{
    if (s->top == 0) return 0;  /* nothing to pop */

    s->top--;
    *out = s->records[s->top];
    return 1;
}
