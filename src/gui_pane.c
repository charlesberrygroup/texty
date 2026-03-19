/*
 * gui_pane.c — Split Pane Layout Implementation
 * =============================================================================
 * See gui_pane.h for the data structure overview and API documentation.
 *
 * KEY DESIGN DECISIONS
 * --------------------
 *
 * 1. In-place split:  gui_pane_split() converts a leaf node into a split
 *    node IN PLACE (modifying the node's fields rather than allocating a
 *    new parent).  This means pointers from the tree above still point to
 *    the same address.  The old leaf data is copied into two new child nodes.
 *
 * 2. In-place close:  gui_pane_close() copies the surviving sibling's data
 *    into the parent node (again in-place), then frees the sibling shell
 *    and the closed pane.  This avoids needing parent pointers.
 *
 * 3. No parent pointers:  The tree is navigated top-down only.  This keeps
 *    the structure simple and avoids dangling-pointer issues during split
 *    and close operations.
 *
 * MEMORY OWNERSHIP
 * ----------------
 * Every GuiPaneNode is heap-allocated via calloc.  gui_pane_free() recursively
 * frees the entire subtree.  gui_pane_split() allocates two new children.
 * gui_pane_close() frees the target and sibling shells.
 * =============================================================================
 */

#include "gui_pane.h"

#include <stdlib.h>   /* calloc, free */
#include <string.h>   /* memset (via calloc) */

/* ============================================================================
 * Creation / Destruction
 * ============================================================================ */

GuiPaneNode *gui_pane_new_leaf(int buffer_idx,
                                int cursor_row, int cursor_col, int desired_col,
                                int view_row, int view_col)
{
    /*
     * calloc zeroes all fields, which sets:
     *   is_leaf=0, sel_active=0, first=NULL, second=NULL, etc.
     * We then set is_leaf=1 and the leaf-specific fields.
     */
    GuiPaneNode *p = calloc(1, sizeof(GuiPaneNode));
    if (!p) return NULL;

    p->is_leaf     = 1;
    p->buffer_idx  = buffer_idx;
    p->cursor_row  = cursor_row;
    p->cursor_col  = cursor_col;
    p->desired_col = desired_col;
    p->view_row    = view_row;
    p->view_col    = view_col;

    return p;
}

void gui_pane_free(GuiPaneNode *node)
{
    if (!node) return;

    if (!node->is_leaf) {
        gui_pane_free(node->first);
        gui_pane_free(node->second);
    }
    free(node);
}

/* ============================================================================
 * Layout
 * ============================================================================ */

void gui_pane_layout(GuiPaneNode *node,
                      int x, int y, int w, int h,
                      int divider_px)
{
    if (!node) return;

    /* Store this node's pixel rectangle */
    node->px = x;
    node->py = y;
    node->pw = w;
    node->ph = h;

    if (node->is_leaf) return;

    if (node->vertical) {
        /*
         * Vertical split:  first (left) | divider | second (right)
         *
         * Split the width according to `ratio`.  The divider bar sits
         * between the two children and takes divider_px pixels.
         */
        int usable = w - divider_px;
        int first_w = (int)(usable * node->ratio);
        if (first_w < 1) first_w = 1;
        int second_w = usable - first_w;
        if (second_w < 1) second_w = 1;

        gui_pane_layout(node->first,  x, y,
                         first_w, h, divider_px);
        gui_pane_layout(node->second, x + first_w + divider_px, y,
                         second_w, h, divider_px);
    } else {
        /*
         * Horizontal split:  first (top) / divider / second (bottom)
         *
         * Split the height according to `ratio`.
         */
        int usable = h - divider_px;
        int first_h = (int)(usable * node->ratio);
        if (first_h < 1) first_h = 1;
        int second_h = usable - first_h;
        if (second_h < 1) second_h = 1;

        gui_pane_layout(node->first,  x, y,
                         w, first_h, divider_px);
        gui_pane_layout(node->second, x, y + first_h + divider_px,
                         w, second_h, divider_px);
    }
}

/* ============================================================================
 * Splitting
 * ============================================================================ */

GuiPaneNode *gui_pane_split(GuiPaneNode *pane, int vertical)
{
    if (!pane || !pane->is_leaf) return NULL;

    /*
     * Create two new leaf children, both copies of the current pane.
     * The first child keeps the original view, the second is the "new pane"
     * that will be focused.
     */
    GuiPaneNode *child1 = gui_pane_new_leaf(
        pane->buffer_idx,
        pane->cursor_row, pane->cursor_col, pane->desired_col,
        pane->view_row, pane->view_col);
    if (!child1) return NULL;
    child1->sel_active     = pane->sel_active;
    child1->sel_anchor_row = pane->sel_anchor_row;
    child1->sel_anchor_col = pane->sel_anchor_col;

    GuiPaneNode *child2 = gui_pane_new_leaf(
        pane->buffer_idx,
        pane->cursor_row, pane->cursor_col, pane->desired_col,
        pane->view_row, pane->view_col);
    if (!child2) {
        gui_pane_free(child1);
        return NULL;
    }
    child2->sel_active     = pane->sel_active;
    child2->sel_anchor_row = pane->sel_anchor_row;
    child2->sel_anchor_col = pane->sel_anchor_col;

    /*
     * Convert this leaf INTO a split node (in-place modification).
     *
     * Why in-place?  Because the parent of this node has a pointer to it.
     * If we allocated a new split node, we'd need to update the parent's
     * pointer.  But we don't have parent pointers in our tree.  By modifying
     * in place, the parent's pointer remains valid.
     */
    pane->is_leaf  = 0;
    pane->vertical = vertical;
    pane->ratio    = 0.5f;
    pane->first    = child1;
    pane->second   = child2;

    /* Clear leaf-only fields on the now-internal node (for cleanliness) */
    pane->buffer_idx    = 0;
    pane->cursor_row    = 0;
    pane->cursor_col    = 0;
    pane->desired_col   = 0;
    pane->view_row      = 0;
    pane->view_col      = 0;
    pane->sel_active    = 0;
    pane->sel_anchor_row = 0;
    pane->sel_anchor_col = 0;

    return child2;
}

/* ============================================================================
 * Closing
 * ============================================================================ */

/*
 * find_parent — locate the parent of `target` in the tree.
 *
 * Sets *which_child to 0 if target is the first child, 1 if second.
 * Returns NULL if target is not found (or is the root).
 */
static GuiPaneNode *find_parent(GuiPaneNode *node, GuiPaneNode *target,
                                 int *which_child)
{
    if (!node || node->is_leaf) return NULL;

    if (node->first == target)  { *which_child = 0; return node; }
    if (node->second == target) { *which_child = 1; return node; }

    GuiPaneNode *found = find_parent(node->first, target, which_child);
    if (found) return found;

    return find_parent(node->second, target, which_child);
}

GuiPaneNode *gui_pane_close(GuiPaneNode **root, GuiPaneNode *target)
{
    if (!root || !*root || !target || !target->is_leaf) return NULL;

    /* Can't close the only pane */
    if (*root == target) return NULL;

    int which;
    GuiPaneNode *parent = find_parent(*root, target, &which);
    if (!parent) return NULL;

    /*
     * The sibling is the other child of the parent — the one that survives.
     */
    GuiPaneNode *sibling = (which == 0) ? parent->second : parent->first;

    /*
     * Detach the sibling from the parent so gui_pane_free(target) doesn't
     * accidentally free it, and so `free(sibling)` below only frees the
     * node shell (not its children, which will be adopted by parent).
     */
    if (which == 0)
        parent->second = NULL;
    else
        parent->first = NULL;

    /*
     * Copy the sibling's entire contents into the parent node.
     *
     * This is the key trick: parent keeps its address in memory (so the
     * grandparent's pointer to parent remains valid), but now contains
     * the sibling's data.  If the sibling was a leaf, parent becomes a
     * leaf.  If the sibling was a split, parent becomes that split.
     *
     * The sibling's children (if any) are now owned by parent.
     */
    GuiPaneNode temp = *sibling;
    *parent = temp;

    /*
     * Free the sibling shell (its children are now owned by parent) and
     * the closed pane.
     */
    free(sibling);
    free(target);

    /*
     * Return the first leaf in the replacement subtree as the new active pane.
     */
    GuiPaneNode *result = parent;
    while (!result->is_leaf)
        result = result->first;
    return result;
}

/* ============================================================================
 * Queries
 * ============================================================================ */

GuiPaneNode *gui_pane_find_at(GuiPaneNode *root, int px, int py)
{
    if (!root) return NULL;

    /* Check if the point falls outside this node's rectangle */
    if (px < root->px || px >= root->px + root->pw ||
        py < root->py || py >= root->py + root->ph)
        return NULL;

    /* If this is a leaf, we found our pane */
    if (root->is_leaf) return root;

    /* Search children (first child is checked first = top/left priority) */
    GuiPaneNode *found = gui_pane_find_at(root->first, px, py);
    if (found) return found;
    return gui_pane_find_at(root->second, px, py);
}

int gui_pane_count(GuiPaneNode *root)
{
    if (!root) return 0;
    if (root->is_leaf) return 1;
    return gui_pane_count(root->first) + gui_pane_count(root->second);
}

int gui_pane_collect_leaves(GuiPaneNode *root,
                             GuiPaneNode **out, int max_leaves)
{
    if (!root || max_leaves <= 0) return 0;

    if (root->is_leaf) {
        out[0] = root;
        return 1;
    }

    int n = gui_pane_collect_leaves(root->first, out, max_leaves);
    n += gui_pane_collect_leaves(root->second, out + n, max_leaves - n);
    return n;
}

/* ============================================================================
 * Focus Cycling
 * ============================================================================ */

GuiPaneNode *gui_pane_next(GuiPaneNode *root, GuiPaneNode *current)
{
    if (!root || !current) return current;

    GuiPaneNode *leaves[GUI_PANE_MAX_LEAVES];
    int n = gui_pane_collect_leaves(root, leaves, GUI_PANE_MAX_LEAVES);
    if (n <= 1) return current;

    for (int i = 0; i < n; i++) {
        if (leaves[i] == current)
            return leaves[(i + 1) % n];
    }
    return current;
}

GuiPaneNode *gui_pane_prev(GuiPaneNode *root, GuiPaneNode *current)
{
    if (!root || !current) return current;

    GuiPaneNode *leaves[GUI_PANE_MAX_LEAVES];
    int n = gui_pane_collect_leaves(root, leaves, GUI_PANE_MAX_LEAVES);
    if (n <= 1) return current;

    for (int i = 0; i < n; i++) {
        if (leaves[i] == current)
            return leaves[(i - 1 + n) % n];
    }
    return current;
}
