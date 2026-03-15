/*
 * test_pane.c — Unit tests for pane.c
 * =============================================================================
 * Tests the pane data structures and layout algorithm.
 *
 * These tests verify:
 *   - pane_create() zero-initialises fields and sets search_match to -1
 *   - pane_node_create_leaf() wraps a pane correctly
 *   - pane_layout() assigns correct screen rectangles for various tree shapes
 *   - pane_node_destroy() frees memory without crashing
 * =============================================================================
 */

#include "test_runner.h"
#include "pane.h"

#include <stdlib.h>

/* ============================================================================
 * pane_create
 * ============================================================================ */

TEST(test_pane_create_defaults)
{
    /*
     * A freshly created pane should have all integer fields at 0 except
     * search_match_row/col which are -1 ("no match").
     */
    Pane *p = pane_create();
    ASSERT(p != NULL,                        "pane_create returns non-NULL");
    ASSERT(p->buffer_index == 0,             "buffer_index starts at 0");
    ASSERT(p->cursor_row == 0,               "cursor_row starts at 0");
    ASSERT(p->cursor_col == 0,               "cursor_col starts at 0");
    ASSERT(p->desired_col == 0,              "desired_col starts at 0");
    ASSERT(p->view_row == 0,                 "view_row starts at 0");
    ASSERT(p->view_col == 0,                 "view_col starts at 0");
    ASSERT(p->sel_active == 0,               "no selection active");
    ASSERT(p->search_match_row == -1,        "search_match_row is -1");
    ASSERT(p->search_match_col == -1,        "search_match_col is -1");
    ASSERT(p->region_active == 0,            "no region active");
    ASSERT(p->x == 0,                        "x starts at 0");
    ASSERT(p->y == 0,                        "y starts at 0");
    ASSERT(p->width == 0,                    "width starts at 0");
    ASSERT(p->height == 0,                   "height starts at 0");
    pane_destroy(p);
}

TEST(test_pane_destroy_null_safe)
{
    /* pane_destroy(NULL) should not crash */
    pane_destroy(NULL);
    ASSERT(1, "pane_destroy(NULL) did not crash");
}

/* ============================================================================
 * pane_node_create_leaf
 * ============================================================================ */

TEST(test_node_create_leaf)
{
    Pane *p = pane_create();
    PaneNode *node = pane_node_create_leaf(p);

    ASSERT(node != NULL,                  "node created");
    ASSERT(node->split == SPLIT_NONE,     "leaf node has SPLIT_NONE");
    ASSERT(node->pane == p,               "leaf node holds the pane");
    ASSERT(node->child1 == NULL,          "leaf has no child1");
    ASSERT(node->child2 == NULL,          "leaf has no child2");

    pane_node_destroy(node);  /* frees both node and pane */
}

TEST(test_node_destroy_null_safe)
{
    pane_node_destroy(NULL);
    ASSERT(1, "pane_node_destroy(NULL) did not crash");
}

/* ============================================================================
 * pane_layout — single leaf
 * ============================================================================ */

TEST(test_layout_single_leaf)
{
    /*
     * A single leaf node should get the entire available rectangle.
     */
    Pane *p = pane_create();
    PaneNode *root = pane_node_create_leaf(p);

    pane_layout(root, 10, 5, 80, 24);

    ASSERT(p->x == 10,       "x matches input");
    ASSERT(p->y == 5,        "y matches input");
    ASSERT(p->width == 80,   "width matches input");
    ASSERT(p->height == 24,  "height matches input");

    pane_node_destroy(root);
}

/* ============================================================================
 * pane_layout — horizontal split
 * ============================================================================ */

TEST(test_layout_horizontal_split)
{
    /*
     * A horizontal split with ratio 0.5 and height 21 should give:
     *   usable = 21 - 1 = 20  (1 row for separator)
     *   top    = 10 rows
     *   bottom = 10 rows
     *   separator at y + 10
     */
    Pane *top = pane_create();
    Pane *bot = pane_create();

    PaneNode *root = calloc(1, sizeof(PaneNode));
    root->split  = SPLIT_HORIZONTAL;
    root->ratio  = 0.5f;
    root->child1 = pane_node_create_leaf(top);
    root->child2 = pane_node_create_leaf(bot);

    pane_layout(root, 0, 1, 80, 21);

    /* Top pane: y=1, height=10 */
    ASSERT(top->x == 0,         "top x");
    ASSERT(top->y == 1,         "top y");
    ASSERT(top->width == 80,    "top width");
    ASSERT(top->height == 10,   "top height");

    /* Bottom pane: y = 1 + 10 + 1(sep) = 12, height = 10 */
    ASSERT(bot->x == 0,         "bot x");
    ASSERT(bot->y == 12,        "bot y (after top + separator)");
    ASSERT(bot->width == 80,    "bot width");
    ASSERT(bot->height == 10,   "bot height");

    pane_node_destroy(root);
}

/* ============================================================================
 * pane_layout — vertical split
 * ============================================================================ */

TEST(test_layout_vertical_split)
{
    /*
     * A vertical split with ratio 0.5 and width 81 should give:
     *   usable = 81 - 1 = 80  (1 col for separator)
     *   left   = 40 cols
     *   right  = 40 cols
     */
    Pane *left  = pane_create();
    Pane *right = pane_create();

    PaneNode *root = calloc(1, sizeof(PaneNode));
    root->split  = SPLIT_VERTICAL;
    root->ratio  = 0.5f;
    root->child1 = pane_node_create_leaf(left);
    root->child2 = pane_node_create_leaf(right);

    pane_layout(root, 0, 0, 81, 24);

    /* Left pane: x=0, width=40 */
    ASSERT(left->x == 0,          "left x");
    ASSERT(left->y == 0,          "left y");
    ASSERT(left->width == 40,     "left width");
    ASSERT(left->height == 24,    "left height");

    /* Right pane: x = 0 + 40 + 1(sep) = 41, width = 40 */
    ASSERT(right->x == 41,        "right x (after left + separator)");
    ASSERT(right->y == 0,         "right y");
    ASSERT(right->width == 40,    "right width");
    ASSERT(right->height == 24,   "right height");

    pane_node_destroy(root);
}

/* ============================================================================
 * pane_layout — nested splits (3 panes)
 * ============================================================================ */

TEST(test_layout_nested_three_panes)
{
    /*
     * Layout: vertical split at root, right child is horizontal split.
     *
     *   +----------+---------+
     *   |          |  top_r  |
     *   |  left    +---------+
     *   |          |  bot_r  |
     *   +----------+---------+
     *
     * Root: vertical, ratio=0.5, width=81 → left=40, sep=1, right=40
     * Right child: horizontal, ratio=0.5, height=21 → top=10, sep=1, bot=10
     */
    Pane *left  = pane_create();
    Pane *top_r = pane_create();
    Pane *bot_r = pane_create();

    PaneNode *right_split = calloc(1, sizeof(PaneNode));
    right_split->split  = SPLIT_HORIZONTAL;
    right_split->ratio  = 0.5f;
    right_split->child1 = pane_node_create_leaf(top_r);
    right_split->child2 = pane_node_create_leaf(bot_r);

    PaneNode *root = calloc(1, sizeof(PaneNode));
    root->split  = SPLIT_VERTICAL;
    root->ratio  = 0.5f;
    root->child1 = pane_node_create_leaf(left);
    root->child2 = right_split;

    pane_layout(root, 0, 1, 81, 21);

    /* Left: x=0, y=1, w=40, h=21 */
    ASSERT(left->width == 40,     "left width");
    ASSERT(left->height == 21,    "left height");

    /* Top-right: x=41, y=1, w=40, h=10 */
    ASSERT(top_r->x == 41,       "top-right x");
    ASSERT(top_r->y == 1,        "top-right y");
    ASSERT(top_r->width == 40,   "top-right width");
    ASSERT(top_r->height == 10,  "top-right height");

    /* Bottom-right: x=41, y=12, w=40, h=10 */
    ASSERT(bot_r->x == 41,       "bot-right x");
    ASSERT(bot_r->y == 12,       "bot-right y");
    ASSERT(bot_r->width == 40,   "bot-right width");
    ASSERT(bot_r->height == 10,  "bot-right height");

    pane_node_destroy(root);
}

/* ============================================================================
 * pane_layout — degenerate: not enough space to split
 * ============================================================================ */

TEST(test_layout_too_small_to_split)
{
    /*
     * With only 2 rows total (usable = 1 after separator), there's not enough
     * room for a horizontal split.  child1 should get everything.
     */
    Pane *top = pane_create();
    Pane *bot = pane_create();

    PaneNode *root = calloc(1, sizeof(PaneNode));
    root->split  = SPLIT_HORIZONTAL;
    root->ratio  = 0.5f;
    root->child1 = pane_node_create_leaf(top);
    root->child2 = pane_node_create_leaf(bot);

    pane_layout(root, 0, 0, 80, 2);

    /* child1 gets everything when space is too small */
    ASSERT(top->height == 2,  "child1 gets all space when too small to split");

    pane_node_destroy(root);
}

/* ============================================================================
 * pane_split — splitting a leaf into two children
 * ============================================================================ */

TEST(test_split_horizontal)
{
    /*
     * Split a single pane horizontally.  The original pane should become
     * child1 (top) and a new pane should be child2 (bottom).
     */
    Pane *orig = pane_create();
    orig->buffer_index = 3;
    orig->cursor_row   = 10;
    orig->cursor_col   = 5;
    PaneNode *root = pane_node_create_leaf(orig);

    Pane *new_pane = pane_split(root, orig, SPLIT_HORIZONTAL);

    ASSERT(new_pane != NULL,              "split returns new pane");
    ASSERT(new_pane != orig,              "new pane is different from original");
    ASSERT(root->split == SPLIT_HORIZONTAL, "root is now a horizontal split");
    ASSERT(root->pane == NULL,            "root is no longer a leaf");
    ASSERT(root->child1 != NULL,          "child1 exists");
    ASSERT(root->child2 != NULL,          "child2 exists");
    ASSERT(root->child1->pane == orig,    "child1 holds original pane");
    ASSERT(root->child2->pane == new_pane, "child2 holds new pane");

    /* State should be copied */
    ASSERT(new_pane->buffer_index == 3,   "buffer_index copied");
    ASSERT(new_pane->cursor_row == 10,    "cursor_row copied");
    ASSERT(new_pane->cursor_col == 5,     "cursor_col copied");

    pane_node_destroy(root);
}

TEST(test_split_vertical)
{
    Pane *orig = pane_create();
    PaneNode *root = pane_node_create_leaf(orig);

    Pane *new_pane = pane_split(root, orig, SPLIT_VERTICAL);

    ASSERT(new_pane != NULL,              "split returns new pane");
    ASSERT(root->split == SPLIT_VERTICAL, "root is a vertical split");
    ASSERT(root->child1->pane == orig,    "child1 is original");
    ASSERT(root->child2->pane == new_pane, "child2 is new");

    pane_node_destroy(root);
}

TEST(test_split_then_layout)
{
    /*
     * Split horizontally, then compute layout.
     * Both panes should get valid rectangles.
     */
    Pane *orig = pane_create();
    PaneNode *root = pane_node_create_leaf(orig);

    Pane *new_pane = pane_split(root, orig, SPLIT_HORIZONTAL);
    pane_layout(root, 0, 1, 80, 21);

    /* Top pane: 10 rows, bottom pane: 10 rows, 1 separator */
    ASSERT(orig->height == 10,     "top pane height after split");
    ASSERT(new_pane->height == 10, "bottom pane height after split");
    ASSERT(orig->y == 1,           "top pane starts at y=1");
    ASSERT(new_pane->y == 12,      "bottom pane starts after top + separator");
    ASSERT(orig->width == 80,      "both panes have full width");
    ASSERT(new_pane->width == 80,  "both panes have full width");

    pane_node_destroy(root);
}

/* ============================================================================
 * pane_collect_leaves
 * ============================================================================ */

TEST(test_collect_leaves_single)
{
    Pane *p = pane_create();
    PaneNode *root = pane_node_create_leaf(p);

    Pane *leaves[PANE_MAX];
    int count = 0;
    pane_collect_leaves(root, leaves, &count);

    ASSERT(count == 1,      "1 leaf in single-pane tree");
    ASSERT(leaves[0] == p,  "leaf is the original pane");

    pane_node_destroy(root);
}

TEST(test_collect_leaves_after_split)
{
    Pane *orig = pane_create();
    PaneNode *root = pane_node_create_leaf(orig);
    Pane *new_pane = pane_split(root, orig, SPLIT_VERTICAL);

    Pane *leaves[PANE_MAX];
    int count = 0;
    pane_collect_leaves(root, leaves, &count);

    ASSERT(count == 2,         "2 leaves after one split");
    ASSERT(leaves[0] == orig,  "first leaf is original");
    ASSERT(leaves[1] == new_pane, "second leaf is new pane");

    pane_node_destroy(root);
}

/* ============================================================================
 * main
 * ============================================================================ */

int main(void)
{
    printf("=== test_pane ===\n");

    RUN(test_pane_create_defaults);
    RUN(test_pane_destroy_null_safe);
    RUN(test_node_create_leaf);
    RUN(test_node_destroy_null_safe);
    RUN(test_layout_single_leaf);
    RUN(test_layout_horizontal_split);
    RUN(test_layout_vertical_split);
    RUN(test_layout_nested_three_panes);
    RUN(test_layout_too_small_to_split);
    RUN(test_split_horizontal);
    RUN(test_split_vertical);
    RUN(test_split_then_layout);
    RUN(test_collect_leaves_single);
    RUN(test_collect_leaves_after_split);

    TEST_SUMMARY();
}
