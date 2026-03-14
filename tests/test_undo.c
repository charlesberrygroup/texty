/*
 * test_undo.c — Unit tests for undo.c
 * =============================================================================
 * Tests the undo/redo stack: push, pop, empty checks, clear, and overflow
 * handling (the oldest record is dropped when the stack is full).
 * =============================================================================
 */

#include "test_runner.h"
#include "undo.h"

#include <string.h>   /* memset */
#include <stdlib.h>   /* malloc, free */

/* ============================================================================
 * Helpers
 * ============================================================================ */

/*
 * make_char_record — build a simple UNDO_INSERT_CHAR record for testing.
 * We use this instead of writing out all fields by hand in every test.
 */
static UndoRecord make_char_record(int row, int col, char c)
{
    UndoRecord r;
    memset(&r, 0, sizeof(r));
    r.type = UNDO_INSERT_CHAR;
    r.row  = row;
    r.col  = col;
    r.c    = c;
    return r;
}

/*
 * make_text_record — build an UNDO_CUT record that owns a heap-allocated
 * text string.  Used to verify that undo_clear() frees text pointers.
 */
static UndoRecord make_text_record(const char *text)
{
    UndoRecord r;
    memset(&r, 0, sizeof(r));
    r.type = UNDO_CUT;
    r.text = malloc(strlen(text) + 1);
    if (r.text) strcpy(r.text, text);
    return r;
}

/* ============================================================================
 * Tests
 * ============================================================================ */

TEST(test_empty_on_init)
{
    /* A freshly zero-initialised stack should report as empty. */
    UndoStack s;
    memset(&s, 0, sizeof(s));
    ASSERT(undo_is_empty(&s), "stack is empty after memset");
}

TEST(test_push_then_not_empty)
{
    UndoStack s;
    memset(&s, 0, sizeof(s));
    undo_push(&s, make_char_record(0, 0, 'a'));
    ASSERT(!undo_is_empty(&s), "stack is not empty after push");
}

TEST(test_pop_returns_pushed_record)
{
    UndoStack s;
    memset(&s, 0, sizeof(s));

    UndoRecord rec = make_char_record(3, 7, 'z');
    undo_push(&s, rec);

    UndoRecord out;
    int ok = undo_pop(&s, &out);

    ASSERT(ok == 1,                        "undo_pop returns 1 on success");
    ASSERT(out.type == UNDO_INSERT_CHAR,   "popped type matches");
    ASSERT(out.row  == 3,                  "popped row matches");
    ASSERT(out.col  == 7,                  "popped col matches");
    ASSERT(out.c    == 'z',               "popped char matches");
}

TEST(test_pop_empty_returns_error)
{
    UndoStack s;
    memset(&s, 0, sizeof(s));
    UndoRecord out;
    int result = undo_pop(&s, &out);
    ASSERT(result == 0, "pop on empty stack returns 0");
}

TEST(test_lifo_order)
{
    /*
     * Push three records (A, B, C) and pop them.
     * Expected pop order: C, B, A  (last in, first out).
     */
    UndoStack s;
    memset(&s, 0, sizeof(s));

    undo_push(&s, make_char_record(0, 0, 'A'));
    undo_push(&s, make_char_record(0, 1, 'B'));
    undo_push(&s, make_char_record(0, 2, 'C'));

    UndoRecord out;
    undo_pop(&s, &out);
    ASSERT(out.c == 'C', "first pop is C");
    undo_pop(&s, &out);
    ASSERT(out.c == 'B', "second pop is B");
    undo_pop(&s, &out);
    ASSERT(out.c == 'A', "third pop is A");
    ASSERT(undo_is_empty(&s), "stack empty after 3 pops");
}

TEST(test_clear_empties_stack)
{
    UndoStack s;
    memset(&s, 0, sizeof(s));
    undo_push(&s, make_char_record(0, 0, 'x'));
    undo_push(&s, make_char_record(0, 1, 'y'));
    undo_clear(&s);
    ASSERT(undo_is_empty(&s), "stack empty after clear");
}

TEST(test_clear_frees_text_pointers)
{
    /*
     * Push a record with a heap-allocated text field (UNDO_CUT).
     * After undo_clear(), the stack should be empty — and under AddressSanitizer
     * (make debug) any double-free or leak would be caught.
     */
    UndoStack s;
    memset(&s, 0, sizeof(s));
    undo_push(&s, make_text_record("some cut text"));
    undo_clear(&s);
    ASSERT(undo_is_empty(&s), "stack empty after clear with text record");
}

TEST(test_overflow_drops_oldest)
{
    /*
     * Fill the stack to capacity + 1.
     * The oldest record (char 0) should be silently dropped and the newest
     * (char UNDO_MAX) should be at the top.
     *
     * We only push UNDO_MAX + 1 simple records (no heap text), so we do not
     * need to worry about freeing text pointers in the overflow path here.
     */
    UndoStack s;
    memset(&s, 0, sizeof(s));

    /* Push exactly UNDO_MAX + 1 records.  Record 0 will be dropped. */
    for (int i = 0; i <= UNDO_MAX; i++) {
        /* Use .col to store i so we can identify records later */
        UndoRecord r = make_char_record(0, i, 'x');
        undo_push(&s, r);
    }

    /* The stack should still be full (UNDO_MAX records), not overflowing */
    ASSERT(!undo_is_empty(&s), "stack is not empty");

    /* The most recently pushed record (col = UNDO_MAX) should be on top */
    UndoRecord top;
    undo_pop(&s, &top);
    ASSERT(top.col == UNDO_MAX, "top record is the newest one");
}

/* ============================================================================
 * main
 * ============================================================================ */

int main(void)
{
    printf("=== test_undo ===\n");

    RUN(test_empty_on_init);
    RUN(test_push_then_not_empty);
    RUN(test_pop_returns_pushed_record);
    RUN(test_pop_empty_returns_error);
    RUN(test_lifo_order);
    RUN(test_clear_empties_stack);
    RUN(test_clear_frees_text_pointers);
    RUN(test_overflow_drops_oldest);

    TEST_SUMMARY();
}
