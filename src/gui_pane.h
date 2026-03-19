/*
 * gui_pane.h — Split Pane Layout (Binary Tree)
 * =============================================================================
 * Manages split pane layout for the GUI frontend.  Panes are organized as a
 * binary tree: each node is either a leaf (a single editor view) or a split
 * (two children arranged side-by-side or stacked).
 *
 * Example tree for a 3-pane layout:
 *
 *       [split V]          V = vertical (left | right)
 *        /      \
 *     [pane]  [split H]    H = horizontal (top / bottom)
 *              /      \
 *           [pane]  [pane]
 *
 * Each leaf pane stores its own cursor position, viewport, and which buffer
 * it's showing.  This allows independent scrolling and editing in each pane,
 * even when two panes show the same file.
 *
 * The pixel rectangle for each node is computed by gui_pane_layout() and
 * stored directly in the node.  This is recalculated each frame.
 *
 * IMPORTANT: This module is pure logic with no SDL or ncurses dependency,
 * so it can be tested independently of the GUI.
 * =============================================================================
 */

#ifndef GUI_PANE_H
#define GUI_PANE_H

/* ---- Constants ------------------------------------------------------------ */

/*
 * Maximum number of leaf panes (for collecting into arrays).
 * With 64 slots, you'd need 6 levels of splitting to exceed this.
 */
#define GUI_PANE_MAX_LEAVES  64

/* ---- Data Types ----------------------------------------------------------- */

/*
 * GuiPaneNode — one node in the binary split-pane tree.
 *
 * There are two kinds of nodes:
 *
 *   Leaf (is_leaf == 1):
 *     Represents a single editor pane.  Stores which buffer it shows
 *     and the cursor/viewport/selection state for that view.
 *
 *   Split (is_leaf == 0):
 *     Represents a division of space.  Has two children (first and second)
 *     and a split direction (vertical = left|right, horizontal = top/bottom).
 *     The `ratio` field controls how much space each child gets (0.0 to 1.0,
 *     where 0.5 means an even 50/50 split).
 *
 * All nodes store a pixel rectangle (px, py, pw, ph) that is computed by
 * gui_pane_layout() each frame.
 */
typedef struct GuiPaneNode {
    int is_leaf;           /* 1 = leaf pane, 0 = internal split node         */

    /* ---- Pixel rectangle (set by gui_pane_layout) ---- */
    int px, py, pw, ph;

    /* ---- Leaf-only: per-pane editor state ---- */
    int buffer_idx;        /* index into Editor.buffers[]                    */
    int cursor_row;        /* cursor row in the buffer                       */
    int cursor_col;        /* cursor column in the buffer                    */
    int desired_col;       /* "sticky" column for vertical movement          */
    int view_row;          /* first visible row (vertical scroll offset)     */
    int view_col;          /* first visible col (horizontal scroll offset)   */
    int sel_active;        /* 1 = text selection is active in this pane      */
    int sel_anchor_row;    /* selection anchor row                           */
    int sel_anchor_col;    /* selection anchor column                        */

    /* ---- Internal-only: split configuration ---- */
    int   vertical;        /* 1 = side-by-side (|), 0 = stacked (—)         */
    float ratio;           /* 0.0–1.0: fraction of space for first child    */
    struct GuiPaneNode *first;    /* top or left child                      */
    struct GuiPaneNode *second;   /* bottom or right child                  */
} GuiPaneNode;

/* ---- Creation / Destruction ----------------------------------------------- */

/*
 * gui_pane_new_leaf — allocate a new leaf pane showing the given buffer.
 *
 * All cursor/viewport fields are initialized from the arguments.
 * Selection starts inactive.  Returns NULL on allocation failure.
 */
GuiPaneNode *gui_pane_new_leaf(int buffer_idx,
                                int cursor_row, int cursor_col, int desired_col,
                                int view_row, int view_col);

/*
 * gui_pane_free — recursively free a pane tree.
 *
 * For a leaf, frees just the node.  For a split, frees both children
 * recursively, then the node itself.  Safe to call with NULL.
 */
void gui_pane_free(GuiPaneNode *node);

/* ---- Layout --------------------------------------------------------------- */

/*
 * gui_pane_layout — compute pixel rectangles for every node in the tree.
 *
 * (x, y, w, h) is the available area for this subtree.
 * divider_px is the pixel width of the divider bar between split children.
 *
 * After this call, every node's (px, py, pw, ph) is set.
 */
void gui_pane_layout(GuiPaneNode *node,
                      int x, int y, int w, int h,
                      int divider_px);

/* ---- Splitting / Closing -------------------------------------------------- */

/*
 * gui_pane_split — split a leaf pane into two.
 *
 * The leaf is converted IN PLACE into a split node.  Two new leaf children
 * are created, both copies of the original pane (same buffer, same cursor).
 *
 *   vertical=1  →  left | right   (the new pane appears to the right)
 *   vertical=0  →  top / bottom   (the new pane appears below)
 *
 * Returns a pointer to the NEW (second) child pane — the one the caller
 * should focus.  Returns NULL if the pane is not a leaf or allocation fails.
 *
 * WARNING: The `pane` pointer now points to a split node, NOT a leaf.
 * The caller should update any "active pane" pointer to the return value.
 */
GuiPaneNode *gui_pane_split(GuiPaneNode *pane, int vertical);

/*
 * gui_pane_close — remove a leaf pane from the tree.
 *
 * The closed pane's sibling replaces their shared parent node in the tree.
 * This is done by copying the sibling's data into the parent node (so
 * pointers from grandparent to parent remain valid).
 *
 * `root` is a pointer to the root pointer — it gets updated if the root
 * node's contents change (which happens when closing a direct child of root).
 *
 * Returns the first leaf of the replacement subtree (to use as the new
 * active pane), or NULL if the target can't be closed (e.g., it's the
 * only pane — you can't close the last one).
 */
GuiPaneNode *gui_pane_close(GuiPaneNode **root, GuiPaneNode *target);

/* ---- Queries -------------------------------------------------------------- */

/*
 * gui_pane_find_at — find the leaf pane containing pixel (px, py).
 *
 * Walks the tree and returns the leaf whose rectangle contains the point.
 * Returns NULL if the point is outside all panes.
 */
GuiPaneNode *gui_pane_find_at(GuiPaneNode *root, int px, int py);

/*
 * gui_pane_count — return the number of leaf panes in the tree.
 */
int gui_pane_count(GuiPaneNode *root);

/*
 * gui_pane_collect_leaves — gather all leaf panes into an array.
 *
 * Fills `out` with up to `max_leaves` leaf pointers, in pre-order
 * (left/top first, then right/bottom).  Returns the number written.
 */
int gui_pane_collect_leaves(GuiPaneNode *root,
                             GuiPaneNode **out, int max_leaves);

/* ---- Focus cycling -------------------------------------------------------- */

/*
 * gui_pane_next — return the next leaf pane after `current`.
 *
 * Wraps around to the first pane.  If there's only one pane, returns it.
 */
GuiPaneNode *gui_pane_next(GuiPaneNode *root, GuiPaneNode *current);

/*
 * gui_pane_prev — return the previous leaf pane before `current`.
 *
 * Wraps around to the last pane.  If there's only one pane, returns it.
 */
GuiPaneNode *gui_pane_prev(GuiPaneNode *root, GuiPaneNode *current);

#endif /* GUI_PANE_H */
