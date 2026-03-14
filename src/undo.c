/*
 * undo.c — Undo / Redo Stack Implementation
 * =============================================================================
 * See undo.h for the data structures and API documentation.
 * =============================================================================
 */

#include "undo.h"

#include <string.h>   /* for memmove */

/* ============================================================================
 * undo_clear
 * ============================================================================ */

void undo_clear(UndoStack *s)
{
    /*
     * Setting top to 0 is all we need — it marks the stack as empty.
     * We don't zero out the records array because any stale data will be
     * overwritten before it is read again.
     */
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
         *
         * memmove(dst, src, n_bytes) copies `n_bytes` bytes from `src` to
         * `dst`.  It is safe even when the regions overlap (unlike memcpy).
         *
         * Here we shift every record one slot earlier, effectively discarding
         * records[0] and freeing up the last slot.
         */
        memmove(&s->records[0],
                &s->records[1],
                (UNDO_MAX - 1) * sizeof(UndoRecord));

        s->top--;  /* top now points at the last (now-free) slot */
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
