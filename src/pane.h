/*
 * pane.h — Split-Pane Data Structures
 * =============================================================================
 * A pane is a single editor viewport — it shows one buffer and has its own
 * cursor, viewport, selection state, and screen rectangle.
 *
 * Multiple panes are organized in a binary tree (PaneNode).  Each internal
 * node represents a split (horizontal or vertical) and each leaf node holds
 * a Pane.  The tree is recursively subdivided to compute screen rectangles:
 *
 *       [SPLIT_VERTICAL]
 *        /            \
 *    [Pane A]    [SPLIT_HORIZONTAL]
 *                  /           \
 *              [Pane B]     [Pane C]
 *
 * In the example above, Pane A occupies the left half of the screen.
 * The right half is split top/bottom between Pane B and Pane C.
 *
 * With one pane (the initial state), the tree is a single leaf node.
 * =============================================================================
 */

#ifndef PANE_H
#define PANE_H

/* ---- Constants ------------------------------------------------------------ */

/** Maximum number of panes that can exist at the same time. */
#define PANE_MAX 16

/* ---- Pane struct ---------------------------------------------------------- */

/*
 * Pane — a single editor viewport.
 *
 * Each pane shows one buffer and maintains its own cursor position, scroll
 * offset, and selection state.  When the user splits the screen, a new pane
 * is created with a copy of the current pane's state.
 *
 * The screen rectangle (x, y, width, height) is computed by pane_layout()
 * and describes where on the terminal this pane renders its content.
 */
typedef struct Pane {
    /*
     * buffer_index — which buffer this pane is viewing.
     *
     * This is an index into Editor.buffers[].  Multiple panes can view the
     * same buffer (same index) with different cursor positions.
     */
    int buffer_index;

    /*
     * Cursor position within the buffer (0-based row and column).
     * desired_col remembers the column the user was "aiming for" during
     * vertical movement so the cursor snaps back on longer lines.
     */
    int cursor_row;
    int cursor_col;
    int desired_col;

    /*
     * Viewport — the top-left corner of the visible area.
     * The pane renders lines starting from view_row and columns from view_col.
     */
    int view_row;
    int view_col;

    /*
     * Selection state — same semantics as the old Editor fields.
     * sel_active != 0 means a selection is in progress.
     * The selection spans from the anchor to the cursor position.
     */
    int sel_active;
    int sel_anchor_row;
    int sel_anchor_col;

    /*
     * Search match position — which match is "current" in this pane.
     * Set to -1/-1 when no match has been found yet.
     * The search query itself is global (stored in Editor).
     */
    int search_match_row;
    int search_match_col;

    /*
     * Region highlight (Ctrl+U) — marks a range of lines with a box border.
     * region_active != 0 means a region is being displayed.
     * region_start_row and region_end_row are inclusive, 0-based.
     */
    int region_active;
    int region_start_row;
    int region_end_row;

    /*
     * Screen rectangle — the area of the terminal this pane occupies.
     * Coordinates are absolute terminal positions (row 0 = top of screen).
     * The pane renders its content within [y .. y+height) rows and
     * [x .. x+width) columns.
     *
     * These are set by pane_layout() and should not be modified directly.
     */
    int x;       /* leftmost column */
    int y;       /* topmost row */
    int width;   /* number of columns */
    int height;  /* number of rows */
} Pane;

/* ---- Split direction ------------------------------------------------------ */

/*
 * SplitDir — how a node divides its screen space between children.
 *
 * SPLIT_NONE means the node is a leaf (it has a pane, not children).
 * SPLIT_HORIZONTAL means the space is divided top/bottom.
 * SPLIT_VERTICAL means the space is divided left/right.
 */
typedef enum {
    SPLIT_NONE,           /* leaf node — contains a Pane */
    SPLIT_HORIZONTAL,     /* children stacked top/bottom */
    SPLIT_VERTICAL        /* children side by side left/right */
} SplitDir;

/* ---- PaneNode ------------------------------------------------------------- */

/*
 * PaneNode — a node in the binary split tree.
 *
 * Leaf nodes (split == SPLIT_NONE):
 *   - pane points to the actual Pane struct
 *   - child1 and child2 are NULL
 *
 * Internal nodes (split != SPLIT_NONE):
 *   - pane is NULL
 *   - child1 is the top (horizontal) or left (vertical) child
 *   - child2 is the bottom (horizontal) or right (vertical) child
 *   - ratio controls how much space goes to child1 (0.0 to 1.0)
 */
typedef struct PaneNode {
    SplitDir split;

    Pane *pane;                /* non-NULL only for leaf nodes */

    struct PaneNode *child1;   /* top or left child */
    struct PaneNode *child2;   /* bottom or right child */

    /*
     * ratio — fraction of the parent's space given to child1.
     *
     * 0.5 means an even 50/50 split.  0.3 would give 30% to child1 and
     * 70% to child2.  Only meaningful for internal nodes (split != SPLIT_NONE).
     */
    float ratio;
} PaneNode;

/* ---- Functions ------------------------------------------------------------ */

/**
 * pane_create — allocate and zero-initialise a new Pane.
 *
 * The caller must eventually free it with pane_destroy().
 * Returns NULL on allocation failure.
 *
 * search_match_row and search_match_col are initialised to -1
 * (meaning "no match found yet").
 */
Pane *pane_create(void);

/**
 * pane_destroy — free a Pane allocated by pane_create().
 *
 * Safe to call with NULL (no-op).
 */
void pane_destroy(Pane *p);

/**
 * pane_node_create_leaf — wrap a Pane in a leaf PaneNode.
 *
 * The returned node owns the pane pointer — pane_node_destroy() will
 * free it.  Returns NULL on allocation failure.
 */
PaneNode *pane_node_create_leaf(Pane *p);

/**
 * pane_node_destroy — recursively free a PaneNode tree.
 *
 * Frees all child nodes and their panes.  Safe to call with NULL (no-op).
 */
void pane_node_destroy(PaneNode *node);

/**
 * pane_layout — recursively compute screen rectangles for all leaf panes.
 *
 * Divides the rectangle (x, y, w, h) among the tree's leaf nodes according
 * to each internal node's split direction and ratio.  After this call, every
 * leaf Pane has valid x/y/width/height values.
 *
 * Call this after creating or closing a pane, or when the terminal is resized.
 */
void pane_layout(PaneNode *node, int x, int y, int w, int h);

#endif /* PANE_H */
