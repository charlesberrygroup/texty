/*
 * pane.c — Split-Pane Implementation
 * =============================================================================
 * Implements the functions declared in pane.h.
 *
 * The pane system uses a binary tree to manage screen layout.  Each leaf node
 * is a Pane (an editor viewport), and each internal node is a split that
 * divides its space between two children.
 *
 * Memory ownership:
 *   - pane_create() allocates a Pane on the heap.
 *   - pane_node_create_leaf() allocates a PaneNode and takes ownership of
 *     the Pane pointer passed to it.
 *   - pane_node_destroy() recursively frees all nodes and their panes.
 * =============================================================================
 */

#include "pane.h"

#include <stdlib.h>   /* malloc, free */
#include <string.h>   /* memset */

/* ============================================================================
 * Pane lifecycle
 * ============================================================================ */

Pane *pane_create(void)
{
    /*
     * calloc() allocates memory AND zeroes it out in one call.
     * This is cleaner than malloc() + memset() and ensures all integer fields
     * start at 0 and all pointer fields start at NULL.
     */
    Pane *p = calloc(1, sizeof(Pane));
    if (!p) return NULL;

    /*
     * search_match_row/col must be -1 (not 0) to mean "no match found yet".
     * Row 0, col 0 is a valid buffer position and would be misinterpreted.
     */
    p->search_match_row = -1;
    p->search_match_col = -1;

    return p;
}

void pane_destroy(Pane *p)
{
    /*
     * free(NULL) is defined as a no-op by the C standard, so this function
     * is safe to call with a NULL argument.
     */
    free(p);
}

/* ============================================================================
 * PaneNode lifecycle
 * ============================================================================ */

PaneNode *pane_node_create_leaf(Pane *p)
{
    /*
     * Allocate a PaneNode and configure it as a leaf.
     * A leaf node has split == SPLIT_NONE, pane != NULL, and no children.
     */
    PaneNode *node = calloc(1, sizeof(PaneNode));
    if (!node) return NULL;

    node->split  = SPLIT_NONE;
    node->pane   = p;
    node->child1 = NULL;
    node->child2 = NULL;
    node->ratio  = 0.5f;   /* default 50/50 — not used for leaf nodes */

    return node;
}

void pane_node_destroy(PaneNode *node)
{
    if (!node) return;

    /*
     * Recurse into children first (post-order traversal).
     * This ensures we free the deepest nodes before their parents,
     * which avoids dangling pointers.
     */
    pane_node_destroy(node->child1);
    pane_node_destroy(node->child2);

    /*
     * If this is a leaf node, free the pane it owns.
     * Internal nodes have pane == NULL, so this is a no-op for them.
     */
    pane_destroy(node->pane);

    free(node);
}

/* ============================================================================
 * Layout — recursive screen rectangle computation
 * ============================================================================ */

void pane_layout(PaneNode *node, int x, int y, int w, int h)
{
    if (!node) return;

    if (node->split == SPLIT_NONE) {
        /*
         * Leaf node: assign the entire available rectangle to this pane.
         * The pane will use these coordinates to know where on the terminal
         * to draw its content.
         */
        if (node->pane) {
            node->pane->x      = x;
            node->pane->y      = y;
            node->pane->width  = w;
            node->pane->height = h;
        }
        return;
    }

    if (node->split == SPLIT_HORIZONTAL) {
        /*
         * Horizontal split: children are stacked top/bottom.
         *
         * child1 gets the top portion (ratio * height rows).
         * child2 gets the bottom portion (remaining rows).
         *
         * One row is consumed by the separator line between them.
         * If there isn't enough space for a separator plus both children,
         * we give everything to child1 (degenerate case).
         *
         *   +------------------+
         *   |     child1       |  top_h rows
         *   +------------------+  <-- 1 row separator (ACS_HLINE)
         *   |     child2       |  bottom_h rows
         *   +------------------+
         */
        int usable = h - 1;   /* subtract 1 row for the separator */
        if (usable < 2) {
            /* Not enough space to split — give everything to child1 */
            pane_layout(node->child1, x, y, w, h);
            return;
        }

        int top_h = (int)(usable * node->ratio);
        if (top_h < 1) top_h = 1;
        if (top_h > usable - 1) top_h = usable - 1;

        int bottom_h = usable - top_h;

        pane_layout(node->child1, x, y, w, top_h);
        /* separator is at row y + top_h (drawn by display.c) */
        pane_layout(node->child2, x, y + top_h + 1, w, bottom_h);

    } else {
        /*
         * Vertical split: children are side by side left/right.
         *
         * child1 gets the left portion (ratio * width columns).
         * child2 gets the right portion (remaining columns).
         *
         * One column is consumed by the separator line between them.
         *
         *   +----------+---+----------+
         *   |  child1  | | |  child2  |
         *   |          | | |          |
         *   +----------+---+----------+
         *               ^ 1 col separator (ACS_VLINE)
         */
        int usable = w - 1;   /* subtract 1 column for the separator */
        if (usable < 2) {
            pane_layout(node->child1, x, y, w, h);
            return;
        }

        int left_w = (int)(usable * node->ratio);
        if (left_w < 1) left_w = 1;
        if (left_w > usable - 1) left_w = usable - 1;

        int right_w = usable - left_w;

        pane_layout(node->child1, x, y, left_w, h);
        /* separator is at column x + left_w (drawn by display.c) */
        pane_layout(node->child2, x + left_w + 1, y, right_w, h);
    }
}
