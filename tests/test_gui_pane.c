/*
 * test_gui_pane.c — Unit tests for gui_pane.c (split pane tree logic)
 * =============================================================================
 * Tests the pure-logic pane tree operations: creation, layout, splitting,
 * closing, finding by position, counting, and focus cycling.
 *
 * These tests do NOT require SDL or ncurses — the pane module is pure C.
 *
 * Compile and run via: make test
 * =============================================================================
 */

#include "test_runner.h"
#include "gui_pane.h"

#include <stdlib.h>   /* NULL */

/* ============================================================================
 * Creation / Destruction
 * ============================================================================ */

TEST(test_new_leaf_basic)
{
    /* A new leaf should have the correct buffer index and cursor state. */
    GuiPaneNode *p = gui_pane_new_leaf(2, 10, 5, 5, 3, 0);
    ASSERT(p != NULL,             "gui_pane_new_leaf returned non-NULL");
    ASSERT(p->is_leaf == 1,       "new pane is a leaf");
    ASSERT(p->buffer_idx == 2,    "buffer_idx is 2");
    ASSERT(p->cursor_row == 10,   "cursor_row is 10");
    ASSERT(p->cursor_col == 5,    "cursor_col is 5");
    ASSERT(p->desired_col == 5,   "desired_col is 5");
    ASSERT(p->view_row == 3,      "view_row is 3");
    ASSERT(p->view_col == 0,      "view_col is 0");
    ASSERT(p->sel_active == 0,    "selection starts inactive");
    ASSERT(p->first == NULL,      "no first child");
    ASSERT(p->second == NULL,     "no second child");
    gui_pane_free(p);
}

TEST(test_free_null)
{
    /* gui_pane_free(NULL) should not crash. */
    gui_pane_free(NULL);
    ASSERT(1, "gui_pane_free(NULL) did not crash");
}

/* ============================================================================
 * Layout
 * ============================================================================ */

TEST(test_layout_single_leaf)
{
    /* A single leaf gets the entire rectangle. */
    GuiPaneNode *p = gui_pane_new_leaf(0, 0, 0, 0, 0, 0);
    gui_pane_layout(p, 100, 50, 800, 600, 2);
    ASSERT(p->px == 100,  "px = 100");
    ASSERT(p->py == 50,   "py = 50");
    ASSERT(p->pw == 800,  "pw = 800");
    ASSERT(p->ph == 600,  "ph = 600");
    gui_pane_free(p);
}

TEST(test_layout_vertical_split)
{
    /*
     * A vertical split (left | right) with ratio 0.5 should divide the
     * width roughly evenly, minus the divider.
     */
    GuiPaneNode *p = gui_pane_new_leaf(0, 0, 0, 0, 0, 0);
    GuiPaneNode *child2 = gui_pane_split(p, 1);  /* vertical */
    ASSERT(child2 != NULL, "split succeeded");

    gui_pane_layout(p, 0, 0, 100, 200, 2);

    /* First child (left): x=0, w = ~49 */
    GuiPaneNode *left = p->first;
    GuiPaneNode *right = p->second;

    ASSERT(left->px == 0,         "left px = 0");
    ASSERT(left->py == 0,         "left py = 0");
    ASSERT(left->ph == 200,       "left ph = full height");

    /* Right starts after left + divider */
    ASSERT(right->px == left->pw + 2, "right starts after left + divider");
    ASSERT(right->py == 0,            "right py = 0");
    ASSERT(right->ph == 200,          "right ph = full height");

    /* Total width should add up: left + divider + right = 100 */
    ASSERT(left->pw + 2 + right->pw == 100,
           "left + divider + right = total width");

    gui_pane_free(p);
}

TEST(test_layout_horizontal_split)
{
    /* A horizontal split (top / bottom) divides height. */
    GuiPaneNode *p = gui_pane_new_leaf(0, 0, 0, 0, 0, 0);
    GuiPaneNode *child2 = gui_pane_split(p, 0);  /* horizontal */
    ASSERT(child2 != NULL, "split succeeded");

    gui_pane_layout(p, 0, 0, 300, 100, 4);

    GuiPaneNode *top = p->first;
    GuiPaneNode *bottom = p->second;

    ASSERT(top->px == 0,          "top px = 0");
    ASSERT(top->pw == 300,        "top pw = full width");
    ASSERT(bottom->px == 0,       "bottom px = 0");
    ASSERT(bottom->pw == 300,     "bottom pw = full width");

    /* Total height should add up */
    ASSERT(top->ph + 4 + bottom->ph == 100,
           "top + divider + bottom = total height");

    /* Bottom starts after top + divider */
    ASSERT(bottom->py == top->ph + 4, "bottom starts after top + divider");

    gui_pane_free(p);
}

/* ============================================================================
 * Splitting
 * ============================================================================ */

TEST(test_split_creates_two_children)
{
    GuiPaneNode *p = gui_pane_new_leaf(3, 15, 7, 7, 5, 2);
    GuiPaneNode *child2 = gui_pane_split(p, 1);  /* vertical */

    ASSERT(child2 != NULL,        "split returned new pane");
    ASSERT(p->is_leaf == 0,       "original node is now a split");
    ASSERT(p->vertical == 1,      "split is vertical");
    ASSERT(p->first != NULL,      "first child exists");
    ASSERT(p->second != NULL,     "second child exists");
    ASSERT(p->second == child2,   "returned pane is the second child");

    /* Both children should copy the original state */
    ASSERT(p->first->is_leaf == 1,       "first child is a leaf");
    ASSERT(p->first->buffer_idx == 3,    "first child buffer = 3");
    ASSERT(p->first->cursor_row == 15,   "first child cursor_row = 15");
    ASSERT(p->first->view_row == 5,      "first child view_row = 5");

    ASSERT(child2->is_leaf == 1,          "second child is a leaf");
    ASSERT(child2->buffer_idx == 3,       "second child buffer = 3");
    ASSERT(child2->cursor_row == 15,      "second child cursor_row = 15");
    ASSERT(child2->cursor_col == 7,       "second child cursor_col = 7");

    gui_pane_free(p);
}

TEST(test_split_non_leaf_fails)
{
    /* Can't split a node that's already a split. */
    GuiPaneNode *p = gui_pane_new_leaf(0, 0, 0, 0, 0, 0);
    gui_pane_split(p, 1);  /* now p is a split */

    GuiPaneNode *result = gui_pane_split(p, 0);
    ASSERT(result == NULL, "splitting a non-leaf returns NULL");

    gui_pane_free(p);
}

TEST(test_split_preserves_selection)
{
    GuiPaneNode *p = gui_pane_new_leaf(0, 5, 3, 3, 0, 0);
    p->sel_active = 1;
    p->sel_anchor_row = 2;
    p->sel_anchor_col = 8;

    GuiPaneNode *child2 = gui_pane_split(p, 0);  /* horizontal */

    ASSERT(p->first->sel_active == 1,       "first child preserves selection");
    ASSERT(p->first->sel_anchor_row == 2,   "first child anchor_row");
    ASSERT(p->first->sel_anchor_col == 8,   "first child anchor_col");
    ASSERT(child2->sel_active == 1,          "second child preserves selection");
    ASSERT(child2->sel_anchor_row == 2,      "second child anchor_row");

    gui_pane_free(p);
}

/* ============================================================================
 * Closing
 * ============================================================================ */

TEST(test_close_only_pane_fails)
{
    /* Can't close the only pane. */
    GuiPaneNode *root = gui_pane_new_leaf(0, 0, 0, 0, 0, 0);
    GuiPaneNode *result = gui_pane_close(&root, root);
    ASSERT(result == NULL,    "close returns NULL for only pane");
    ASSERT(root->is_leaf,     "root is still a leaf");
    gui_pane_free(root);
}

TEST(test_close_second_child)
{
    /*
     * Split into two, then close the second child.
     * The root should become a leaf again (the first child's data).
     */
    GuiPaneNode *root = gui_pane_new_leaf(0, 10, 5, 5, 0, 0);
    GuiPaneNode *child2 = gui_pane_split(root, 1);
    ASSERT(gui_pane_count(root) == 2, "two panes after split");

    /* Change child1's state so we can verify it survives */
    root->first->cursor_row = 42;

    GuiPaneNode *result = gui_pane_close(&root, child2);
    ASSERT(result != NULL,          "close succeeded");
    ASSERT(gui_pane_count(root) == 1, "one pane after close");
    ASSERT(root->is_leaf,           "root is a leaf after close");
    ASSERT(root->cursor_row == 42,  "surviving pane has cursor_row = 42");

    gui_pane_free(root);
}

TEST(test_close_first_child)
{
    /* Split into two, then close the first child.
     * The second child's data should replace the root. */
    GuiPaneNode *root = gui_pane_new_leaf(0, 0, 0, 0, 0, 0);
    GuiPaneNode *child2 = gui_pane_split(root, 1);
    GuiPaneNode *child1 = root->first;

    /* Give child2 distinctive state */
    child2->cursor_row = 99;
    child2->buffer_idx = 5;

    GuiPaneNode *result = gui_pane_close(&root, child1);
    ASSERT(result != NULL,          "close succeeded");
    ASSERT(gui_pane_count(root) == 1, "one pane after close");
    ASSERT(root->is_leaf,           "root is a leaf");
    ASSERT(root->cursor_row == 99,  "surviving pane cursor_row = 99");
    ASSERT(root->buffer_idx == 5,   "surviving pane buffer_idx = 5");

    gui_pane_free(root);
}

TEST(test_close_in_nested_tree)
{
    /*
     * Create a 3-pane layout:
     *     root (V split)
     *      /          \
     *   paneA      split (H split)
     *                /      \
     *            paneB     paneC
     *
     * Close paneB.  paneC should replace the inner split.
     * Result: root (V split) → paneA | paneC
     */
    GuiPaneNode *root = gui_pane_new_leaf(0, 0, 0, 0, 0, 0);
    GuiPaneNode *right_pane = gui_pane_split(root, 1);  /* V split */
    ASSERT(gui_pane_count(root) == 2, "2 panes after first split");

    /* Now split the right pane horizontally */
    GuiPaneNode *paneC = gui_pane_split(right_pane, 0);  /* H split */
    ASSERT(gui_pane_count(root) == 3, "3 panes after second split");

    /*
     * right_pane is now an internal node.  Its first child is paneB
     * (the original right_pane data), its second child is paneC.
     */
    GuiPaneNode *inner_split = root->second;
    GuiPaneNode *paneB = inner_split->first;
    paneB->cursor_row = 11;
    paneC->cursor_row = 22;

    /* Close paneB */
    GuiPaneNode *result = gui_pane_close(&root, paneB);
    ASSERT(result != NULL,              "close succeeded");
    ASSERT(gui_pane_count(root) == 2,   "2 panes after close");

    /* The inner split should now be a leaf (paneC's data) */
    ASSERT(root->second->is_leaf == 1,      "right side is a leaf");
    ASSERT(root->second->cursor_row == 22,  "right pane has paneC's state");

    gui_pane_free(root);
}

/* ============================================================================
 * Finding pane by position
 * ============================================================================ */

TEST(test_find_at_single_pane)
{
    GuiPaneNode *p = gui_pane_new_leaf(0, 0, 0, 0, 0, 0);
    gui_pane_layout(p, 0, 0, 100, 100, 2);

    ASSERT(gui_pane_find_at(p, 50, 50) == p,  "center hit");
    ASSERT(gui_pane_find_at(p, 0, 0) == p,    "top-left corner hit");
    ASSERT(gui_pane_find_at(p, 99, 99) == p,  "bottom-right corner hit");
    ASSERT(gui_pane_find_at(p, 100, 50) == NULL, "just outside right edge");
    ASSERT(gui_pane_find_at(p, -1, 50) == NULL,  "outside left edge");

    gui_pane_free(p);
}

TEST(test_find_at_split_panes)
{
    GuiPaneNode *root = gui_pane_new_leaf(0, 0, 0, 0, 0, 0);
    gui_pane_split(root, 1);  /* vertical split */
    gui_pane_layout(root, 0, 0, 100, 100, 2);

    GuiPaneNode *left = root->first;
    GuiPaneNode *right = root->second;

    /* Click in the left half should find the left pane */
    GuiPaneNode *found = gui_pane_find_at(root, 10, 50);
    ASSERT(found == left, "left half finds left pane");

    /* Click in the right half should find the right pane */
    found = gui_pane_find_at(root, 90, 50);
    ASSERT(found == right, "right half finds right pane");

    gui_pane_free(root);
}

/* ============================================================================
 * Counting
 * ============================================================================ */

TEST(test_count_single)
{
    GuiPaneNode *p = gui_pane_new_leaf(0, 0, 0, 0, 0, 0);
    ASSERT(gui_pane_count(p) == 1, "1 pane");
    gui_pane_free(p);
}

TEST(test_count_after_splits)
{
    GuiPaneNode *root = gui_pane_new_leaf(0, 0, 0, 0, 0, 0);
    gui_pane_split(root, 1);
    ASSERT(gui_pane_count(root) == 2, "2 panes after 1 split");

    gui_pane_split(root->first, 0);
    ASSERT(gui_pane_count(root) == 3, "3 panes after 2 splits");

    gui_pane_split(root->second, 1);
    ASSERT(gui_pane_count(root) == 4, "4 panes after 3 splits");

    gui_pane_free(root);
}

TEST(test_count_null)
{
    ASSERT(gui_pane_count(NULL) == 0, "count of NULL is 0");
}

/* ============================================================================
 * Collect leaves
 * ============================================================================ */

TEST(test_collect_leaves)
{
    GuiPaneNode *root = gui_pane_new_leaf(0, 0, 0, 0, 0, 0);
    gui_pane_split(root, 1);           /* V split: left | right */
    gui_pane_split(root->first, 0);    /* H split on left: top / bottom */

    /* Tree is now:
     *     root (V)
     *      /       \
     *   split(H)   paneC
     *    / \
     * paneA paneB
     */

    GuiPaneNode *leaves[8];
    int n = gui_pane_collect_leaves(root, leaves, 8);
    ASSERT(n == 3, "3 leaves collected");

    /* Pre-order: left subtree first → paneA, paneB, then paneC */
    GuiPaneNode *paneA = root->first->first;
    GuiPaneNode *paneB = root->first->second;
    GuiPaneNode *paneC = root->second;

    ASSERT(leaves[0] == paneA, "first leaf is paneA");
    ASSERT(leaves[1] == paneB, "second leaf is paneB");
    ASSERT(leaves[2] == paneC, "third leaf is paneC");

    gui_pane_free(root);
}

/* ============================================================================
 * Focus cycling
 * ============================================================================ */

TEST(test_next_single_pane)
{
    GuiPaneNode *p = gui_pane_new_leaf(0, 0, 0, 0, 0, 0);
    ASSERT(gui_pane_next(p, p) == p, "next of single pane is itself");
    gui_pane_free(p);
}

TEST(test_next_two_panes)
{
    GuiPaneNode *root = gui_pane_new_leaf(0, 0, 0, 0, 0, 0);
    GuiPaneNode *child2 = gui_pane_split(root, 1);
    GuiPaneNode *child1 = root->first;

    ASSERT(gui_pane_next(root, child1) == child2, "next of first is second");
    ASSERT(gui_pane_next(root, child2) == child1, "next of second wraps to first");
    gui_pane_free(root);
}

TEST(test_prev_two_panes)
{
    GuiPaneNode *root = gui_pane_new_leaf(0, 0, 0, 0, 0, 0);
    GuiPaneNode *child2 = gui_pane_split(root, 1);
    GuiPaneNode *child1 = root->first;

    ASSERT(gui_pane_prev(root, child2) == child1, "prev of second is first");
    ASSERT(gui_pane_prev(root, child1) == child2, "prev of first wraps to second");
    gui_pane_free(root);
}

TEST(test_next_three_panes)
{
    GuiPaneNode *root = gui_pane_new_leaf(0, 0, 0, 0, 0, 0);
    gui_pane_split(root, 1);           /* A | B */
    gui_pane_split(root->second, 0);   /* A | (B / C) */

    GuiPaneNode *a = root->first;
    GuiPaneNode *b = root->second->first;
    GuiPaneNode *c = root->second->second;

    ASSERT(gui_pane_next(root, a) == b, "A → B");
    ASSERT(gui_pane_next(root, b) == c, "B → C");
    ASSERT(gui_pane_next(root, c) == a, "C → A (wrap)");

    gui_pane_free(root);
}

/* ============================================================================
 * main — run all tests
 * ============================================================================ */

int main(void)
{
    printf("=== gui_pane tests ===\n");

    /* Creation */
    RUN(test_new_leaf_basic);
    RUN(test_free_null);

    /* Layout */
    RUN(test_layout_single_leaf);
    RUN(test_layout_vertical_split);
    RUN(test_layout_horizontal_split);

    /* Splitting */
    RUN(test_split_creates_two_children);
    RUN(test_split_non_leaf_fails);
    RUN(test_split_preserves_selection);

    /* Closing */
    RUN(test_close_only_pane_fails);
    RUN(test_close_second_child);
    RUN(test_close_first_child);
    RUN(test_close_in_nested_tree);

    /* Find at position */
    RUN(test_find_at_single_pane);
    RUN(test_find_at_split_panes);

    /* Counting */
    RUN(test_count_single);
    RUN(test_count_after_splits);
    RUN(test_count_null);

    /* Collect leaves */
    RUN(test_collect_leaves);

    /* Focus cycling */
    RUN(test_next_single_pane);
    RUN(test_next_two_panes);
    RUN(test_prev_two_panes);
    RUN(test_next_three_panes);

    TEST_SUMMARY();
}
