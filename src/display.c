/*
 * display.c — Terminal Rendering Implementation
 * =============================================================================
 * See display.h for the public API.
 *
 * IMPORTANT: This file includes <ncurses.h>.  On most systems you do NOT need
 * to include anything else.  On macOS, ncurses.h is at /usr/include/ncurses.h.
 * On Linux, install libncurses-dev (Debian/Ubuntu) or ncurses-devel (Fedora).
 * =============================================================================
 */

#include "display.h"
#include "editor.h"
#include "buffer.h"
#include "syntax.h"
#include "filetree.h"   /* for FileTree, FlatEntry, filetree_is_expanded */

#include <ncurses.h>
#include <string.h>
#include <stdio.h>

/* ============================================================================
 * display_init
 * ============================================================================ */

void display_init(void)
{
    /*
     * initscr() starts ncurses.  After this call, stdscr is valid and we can
     * use all ncurses functions.
     */
    initscr();

    /*
     * raw() is like cbreak() — characters are available immediately without
     * waiting for Enter, and noecho() prevents them from appearing on screen.
     *
     * The key difference from cbreak(): raw() ALSO disables XON/XOFF flow
     * control.  On macOS, the terminal intercepts Ctrl+Q (XOFF) and Ctrl+S
     * (XON) for flow control before the app can see them.  raw() turns that
     * off so those keystrokes reach us.  (The trade-off: Ctrl+C no longer
     * sends SIGINT, but that is fine for a text editor with its own quit key.)
     */
    raw();
    noecho();

    /*
     * keypad(stdscr, TRUE) enables ncurses to interpret multi-byte escape
     * sequences (like arrow keys) and return them as named KEY_* constants.
     */
    keypad(stdscr, TRUE);

    /*
     * nodelay(stdscr, FALSE) makes getch() block until a key is pressed.
     * (FALSE = blocking mode — the default, but we set it explicitly.)
     */
    nodelay(stdscr, FALSE);

    /*
     * Set up colors.  has_colors() returns true if the terminal supports color.
     */
    if (has_colors()) {
        start_color();
        use_default_colors();  /* -1 means "terminal default" background */

        /*
         * init_pair(pair_id, foreground, background)
         * COLOR_* constants: BLACK, RED, GREEN, YELLOW, BLUE, MAGENTA, CYAN, WHITE
         * -1 = terminal default color
         *
         * NOTE: We do NOT define CPAIR_DEFAULT here because on macOS the
         * system ncurses has a bug where -1 (terminal default) gets treated as
         * COLOR_BLACK when use_default_colors() does not work correctly.
         * Instead, for normal text we call attrset(A_NORMAL) to clear all
         * attributes and let the terminal draw in its own default colors.
         *
         * NOTE: CPAIR_CURLINE is also not defined here for the same reason —
         * we use A_REVERSE (reverse video) for the cursor-line highlight
         * instead, which just swaps whatever foreground/background the terminal
         * already has.  This works correctly on any terminal and color scheme.
         */
        init_pair(CPAIR_GUTTER,       COLOR_CYAN,    -1);
        init_pair(CPAIR_STATUS,       COLOR_BLACK,   COLOR_CYAN);
        init_pair(CPAIR_STATUS_DIRTY, COLOR_BLACK,   COLOR_YELLOW);
        /*
         * CPAIR_SELECTION — selected text.
         * We use explicit colors (no -1) to avoid the macOS ncurses bug
         * described above.  Black text on cyan is visible on both light
         * and dark terminal backgrounds, and is distinct from the gutter.
         */
        init_pair(CPAIR_SELECTION,    COLOR_BLACK,   COLOR_CYAN);

        /*
         * Tab bar colors.
         * CPAIR_TAB_ACTIVE   — the tab for the currently open buffer.
         * CPAIR_TAB_INACTIVE — all other tabs.
         *
         * We use explicit COLOR_* constants (no -1) to avoid the macOS bug
         * where -1 maps to COLOR_BLACK.
         */
        init_pair(CPAIR_TAB_ACTIVE,   COLOR_WHITE,   COLOR_BLUE);
        init_pair(CPAIR_TAB_INACTIVE, COLOR_BLACK,   COLOR_WHITE);

        /*
         * Search-match highlight colors.
         * CPAIR_SEARCH_MATCH — every match of the search query (yellow bg).
         * CPAIR_SEARCH_CUR   — the match the cursor is sitting on (green bg).
         */
        init_pair(CPAIR_SEARCH_MATCH, COLOR_BLACK,   COLOR_YELLOW);
        init_pair(CPAIR_SEARCH_CUR,   COLOR_BLACK,   COLOR_GREEN);

        /*
         * CPAIR_BRACKET — matching bracket pair highlight.
         * White text on magenta is visually distinct from search (yellow/green)
         * and selection (cyan), and stands out on both light and dark terminals.
         */
        init_pair(CPAIR_BRACKET,      COLOR_WHITE,   COLOR_MAGENTA);

        /*
         * Syntax-highlighting color pairs.
         * These use -1 (terminal default) as the background so syntax colors
         * blend naturally with whatever background color the user has set.
         *
         * Color choices were made to be readable on both dark and light
         * terminal backgrounds and to not clash with existing UI colors:
         *   KEYWORD  — bold yellow  (stands out, commonly used for keywords)
         *   TYPE     — cyan         (already used for gutter; distinct enough)
         *   STRING   — green        (universal convention for string literals)
         *   COMMENT  — blue         (reads as "less important" than code)
         *   PREPROC  — magenta      (already used for brackets; good contrast)
         *   NUMBER   — red          (numerics in red is a common convention)
         */
        init_pair(CPAIR_SYN_KEYWORD,  COLOR_YELLOW,  -1);
        init_pair(CPAIR_SYN_TYPE,     COLOR_CYAN,    -1);
        init_pair(CPAIR_SYN_STRING,   COLOR_GREEN,   -1);
        init_pair(CPAIR_SYN_COMMENT,  COLOR_BLUE,    -1);
        init_pair(CPAIR_SYN_PREPROC,  COLOR_MAGENTA, -1);
        init_pair(CPAIR_SYN_NUMBER,   COLOR_RED,     -1);

        /*
         * File explorer panel color pairs.
         *
         * CPAIR_FILETREE_DIR    — directories: blue foreground, terminal default bg.
         * CPAIR_FILETREE_FILE   — files: terminal default fg and bg (plain text).
         * CPAIR_FILETREE_CURSOR — cursor row when tree has focus: black on white,
         *                         visible on both dark and light terminals.
         */
        init_pair(CPAIR_FILETREE_DIR,    COLOR_BLUE,  -1);
        init_pair(CPAIR_FILETREE_FILE,   -1,          -1);
        init_pair(CPAIR_FILETREE_CURSOR, COLOR_BLACK, COLOR_WHITE);

        /*
         * CPAIR_REGION — used for the region box border drawn by Ctrl+U.
         * Red foreground on the terminal's default background, so the border
         * is visible on both dark and light terminals without changing the
         * background color of any text.
         */
        init_pair(CPAIR_REGION, COLOR_RED, -1);

        /*
         * CPAIR_PANE_BORDER — separator lines drawn between split panes.
         * White on default background so it is visible but not distracting.
         */
        init_pair(CPAIR_PANE_BORDER, COLOR_WHITE, -1);
    }
}

/* ============================================================================
 * display_cleanup
 * ============================================================================ */

void display_cleanup(void)
{
    /*
     * endwin() restores the terminal to the state it was in before initscr()
     * was called.  Always call this before exiting.
     */
    endwin();
}

/* ============================================================================
 * display_update_size
 * ============================================================================ */

void display_update_size(struct Editor *ed)
{
    /*
     * getmaxyx(win, y, x) fills y and x with the window dimensions.
     * Note: it is a macro, not a function — do NOT take the address of y/x.
     */
    getmaxyx(stdscr, ed->term_rows, ed->term_cols);
}

/* ============================================================================
 * Internal: extract just the filename from a full path
 * ============================================================================ */

/*
 * path_basename — return a pointer to the filename portion of `path`.
 *
 * Examples:
 *   "/home/user/foo.c"  →  "foo.c"
 *   "foo.c"             →  "foo.c"
 *
 * This avoids the platform-specific behaviour of the POSIX basename() function
 * (which may modify its argument or return a pointer to static storage).
 * We simply find the last '/' and return the character after it.
 */
static const char *path_basename(const char *path)
{
    const char *p = strrchr(path, '/');
    return p ? p + 1 : path;
}

/* ============================================================================
 * Internal: draw the tab bar (row 0)
 * ============================================================================ */

static void draw_tab_bar(struct Editor *ed)
{
    move(0, 0);

    int x = 0;   /* current column within the tab bar */

    for (int i = 0; i < ed->num_buffers; i++) {
        Buffer     *buf  = ed->buffers[i];
        int         active = (i == ed->active_pane->buffer_index);

        /*
         * Build the tab label: " filename [+] " for dirty buffers,
         * " filename " for clean ones.  Truncate long names with "…".
         */
        const char *name = buf->filename
                         ? path_basename(buf->filename)
                         : "[No Name]";
        const char *dirt = buf->dirty ? " [+]" : "";

        char label[64];
        /* Truncate the name at 20 chars so tabs don't consume the whole bar */
        snprintf(label, sizeof(label), " %.20s%s ", name, dirt);

        int label_len = (int)strlen(label);

        /* Stop if this tab would overflow the terminal width */
        if (x + label_len > ed->term_cols) break;

        /* Choose color based on whether this is the active tab */
        if (active) {
            attron(COLOR_PAIR(CPAIR_TAB_ACTIVE) | A_BOLD);
        } else {
            attron(COLOR_PAIR(CPAIR_TAB_INACTIVE));
        }

        mvprintw(0, x, "%s", label);
        x += label_len;

        if (active) {
            attroff(COLOR_PAIR(CPAIR_TAB_ACTIVE) | A_BOLD);
        } else {
            attroff(COLOR_PAIR(CPAIR_TAB_INACTIVE));
        }

        /* Draw a separator between tabs using the inactive style */
        if (i < ed->num_buffers - 1 && x < ed->term_cols) {
            attron(COLOR_PAIR(CPAIR_TAB_INACTIVE));
            mvaddch(0, x, '|');
            attroff(COLOR_PAIR(CPAIR_TAB_INACTIVE));
            x++;
        }
    }

    /* Fill the remainder of the tab bar row with the inactive tab color */
    attron(COLOR_PAIR(CPAIR_TAB_INACTIVE));
    for (; x < ed->term_cols; x++)
        mvaddch(0, x, ' ');
    attroff(COLOR_PAIR(CPAIR_TAB_INACTIVE));
}

/* ============================================================================
 * Internal: draw the editor area (buffer content + gutter)
 * ============================================================================ */

/* ============================================================================
 * Internal: per-character line renderer (used when search is active)
 * ============================================================================ */

/*
 * col_in_selection — return 1 if buffer position (buf_row, buf_col) falls
 * inside the normalized selection [sel_sr/sc .. sel_er/ec).
 */
static int col_in_selection(int sel_active,
                             int sel_sr, int sel_sc, int sel_er, int sel_ec,
                             int buf_row, int buf_col)
{
    if (!sel_active) return 0;
    if (buf_row < sel_sr || buf_row > sel_er) return 0;
    int row_start = (buf_row == sel_sr) ? sel_sc : 0;
    int row_end   = (buf_row == sel_er) ? sel_ec : (1 << 30);
    return buf_col >= row_start && buf_col < row_end;
}

/*
 * col_in_any_match — return 1 if column buf_col on a line falls inside any
 * occurrence of `query` (length qlen) within `line_text`.
 *
 * A match at start position p covers columns [p, p+qlen).  We check all
 * start positions p where p <= buf_col < p+qlen (i.e. p in
 * [buf_col-qlen+1, buf_col]).
 */
static int col_in_any_match(const char *line_text, int line_len,
                             int buf_col,
                             const char *query, int qlen)
{
    if (qlen <= 0) return 0;

    int p_start = buf_col - qlen + 1;
    if (p_start < 0) p_start = 0;

    for (int p = p_start; p <= buf_col && p + qlen <= line_len; p++) {
        if (strncmp(line_text + p, query, qlen) == 0)
            return 1;
    }
    return 0;
}

/*
 * draw_char_ws — output one character, substituting a visible marker for
 * spaces when show_whitespace is enabled.
 *
 * We use ncurses' ACS_BULLET (a portable middle-dot glyph) for spaces.
 * The caller is responsible for setting the correct attribute before calling
 * this function; we only change the character, not the color or style.
 */
static void draw_char_ws(char c, int show_whitespace)
{
    if (show_whitespace && c == ' ') {
        /*
         * ACS_BULLET is a portable ncurses character for a small filled
         * circle/dot.  It renders correctly on all terminal types that
         * support the ACS (Alternate Character Set) line-drawing characters.
         */
        addch(ACS_BULLET);
    } else {
        /*
         * Cast to unsigned char: characters > 127 would be sign-extended
         * to a negative int without the cast, which ncurses may misinterpret.
         */
        addch((unsigned char)c);
    }
}

/*
 * draw_text_segment — output `len` characters from `text`, substituting
 * spaces with the whitespace marker when show_whitespace is enabled.
 *
 * This replaces addnstr() calls in the non-search rendering path so that
 * whitespace substitution works in all rendering modes uniformly.
 */
static void draw_text_segment(const char *text, int len, int show_whitespace)
{
    for (int i = 0; i < len; i++)
        draw_char_ws(text[i], show_whitespace);
}

/*
 * syntax_token_attr — map a SyntaxToken value to an ncurses attribute.
 *
 * Returns A_NORMAL when `tokens` is NULL (no language detected) or when the
 * token type is SYN_NORMAL (plain text).  The caller should pass `buf_col`
 * as the index into the tokens array for the character being rendered.
 *
 * Keywords get A_BOLD to make them visually heavier without needing a
 * separate foreground color.  All other token types use color only.
 */
static int syntax_token_attr(const SyntaxToken *tokens, int col)
{
    if (tokens == NULL) return A_NORMAL;
    switch (tokens[col]) {
        case SYN_KEYWORD: return COLOR_PAIR(CPAIR_SYN_KEYWORD) | A_BOLD;
        case SYN_TYPE:    return COLOR_PAIR(CPAIR_SYN_TYPE);
        case SYN_STRING:  return COLOR_PAIR(CPAIR_SYN_STRING);
        case SYN_COMMENT: return COLOR_PAIR(CPAIR_SYN_COMMENT);
        case SYN_PREPROC: return COLOR_PAIR(CPAIR_SYN_PREPROC);
        case SYN_NUMBER:  return COLOR_PAIR(CPAIR_SYN_NUMBER);
        default:          return A_NORMAL;
    }
}

/*
 * draw_line_with_search — render one line character-by-character, applying
 * highlights in priority order (highest first):
 *
 *   bracket match  >  current search match  >  other search matches
 *   >  selection  >  cursor-row reverse-video  >  syntax color  >  normal
 *
 * bm_row / bm_col  — position of the matching bracket (-1 if none).
 *   The cursor bracket itself is at (ed->active_pane->cursor_row, ed->active_pane->cursor_col).
 *   Both positions get CPAIR_BRACKET when bracket matching is active.
 *
 * tokens — per-character SyntaxToken array from syntax_highlight_line(),
 *   or NULL when no syntax language is detected for this file.
 */
static void draw_line_with_search(int buf_row,
                                   const char *line_text, int line_len,
                                   const char *draw_start, int draw_len,
                                   int view_col, int row_attr,
                                   int sel_active,
                                   int sel_sr, int sel_sc,
                                   int sel_er, int sel_ec,
                                   int bm_row,  int bm_col,
                                   const SyntaxToken *tokens,
                                   struct Editor *ed)
{
    const char *query      = ed->search_query;
    int         qlen       = (int)strlen(query);
    int         match_row  = ed->active_pane->search_match_row;
    int         match_col  = ed->active_pane->search_match_col;

    for (int i = 0; i < draw_len; i++) {
        int buf_col = view_col + i;

        int attr;

        if ((buf_row == ed->active_pane->cursor_row && buf_col == ed->active_pane->cursor_col
                && buffer_line_len(editor_current_buffer(ed), buf_row) > buf_col
                && bm_col >= 0)
            || (buf_row == bm_row && buf_col == bm_col)) {
            /*
             * This character is one of the two matched brackets.
             * Highest priority — shown in magenta regardless of search/selection.
             */
            attr = COLOR_PAIR(CPAIR_BRACKET) | A_BOLD;

        } else if (match_row == buf_row
                && buf_col >= match_col
                && buf_col < match_col + qlen) {
            /*
             * Current (jumped-to) search match — green.
             */
            attr = COLOR_PAIR(CPAIR_SEARCH_CUR) | A_BOLD;

        } else if (col_in_any_match(line_text, line_len, buf_col, query, qlen)) {
            /*
             * Any other search match — yellow.
             */
            attr = COLOR_PAIR(CPAIR_SEARCH_MATCH);

        } else if (col_in_selection(sel_active,
                                     sel_sr, sel_sc, sel_er, sel_ec,
                                     buf_row, buf_col)) {
            attr = COLOR_PAIR(CPAIR_SELECTION);

        } else if (row_attr != A_NORMAL) {
            /*
             * Cursor row — use reverse video for the whole line.
             * A_REVERSE swaps the terminal's foreground/background colors,
             * which would make syntax colors invisible anyway, so we skip
             * syntax highlighting on the cursor row.
             */
            attr = row_attr;

        } else {
            /*
             * Normal text — apply syntax color.
             * syntax_token_attr() returns A_NORMAL for SYN_NORMAL characters
             * and when tokens is NULL, so plain text is unaffected.
             */
            attr = syntax_token_attr(tokens, buf_col);
        }

        attrset(attr);
        draw_char_ws(draw_start[i], ed->show_whitespace);
    }
}

/*
 * line_screen_rows_d — display-side copy of the same helper in editor.c.
 *
 * Returns the number of screen rows a line of `line_len` chars occupies
 * when word-wrap is on and the text area is `text_cols` columns wide.
 * (We duplicate it here so display.c stays independent of editor.c internals.)
 */
static int line_screen_rows_d(int line_len, int text_cols)
{
    if (text_cols <= 0) return 1;
    if (line_len == 0)  return 1;
    return (line_len + text_cols - 1) / text_cols;
}

/* ============================================================================
 * Internal: draw the file explorer panel (left side panel)
 * ============================================================================ */

/*
 * draw_filetree_panel — render the file tree in the left FILETREE_WIDTH columns.
 *
 * Layout:
 *   Row TAB_BAR_HEIGHT         : header (" Files")
 *   Row TAB_BAR_HEIGHT+1 ...   : tree entries (one per row)
 *   Each row ends with '|' at column FILETREE_WIDTH-1 as a border separator.
 *
 * Scrolling:
 *   filetree_scroll is the index of the first entry shown in the entry area.
 *   We adjust it here so the cursor always stays on screen.
 *
 * Focus:
 *   When filetree_focus == 1, the cursor row uses CPAIR_FILETREE_CURSOR
 *   (black on white, clearly highlighted).
 *   When filetree_focus == 0, the cursor row is only bold (dimmed highlight).
 */
static void draw_filetree_panel(struct Editor *ed)
{
    FileTree *ft = ed->filetree;

    /*
     * panel_rows: total rows available for the panel (tab bar at top, status
     * bar at bottom are reserved).
     *
     * entry_rows: we give the first panel row to the "Files" header, leaving
     * entry_rows rows for actual tree entries.
     */
    int panel_rows = ed->term_rows - TAB_BAR_HEIGHT - 1;
    int entry_rows = panel_rows - 1;  /* one row reserved for header */

    /*
     * Scroll adjustment — keep filetree_cursor visible within entry_rows.
     *
     * If the cursor is above the scroll window, scroll up.
     * If the cursor is at or below the bottom edge, scroll down.
     * Always clamp filetree_scroll to >= 0.
     */
    if (ed->filetree_cursor < ed->filetree_scroll)
        ed->filetree_scroll = ed->filetree_cursor;
    if (ed->filetree_cursor >= ed->filetree_scroll + entry_rows)
        ed->filetree_scroll = ed->filetree_cursor - entry_rows + 1;
    if (ed->filetree_scroll < 0)
        ed->filetree_scroll = 0;

    /* ---- Header row ---- */
    /*
     * move(row, col): position the ncurses cursor before drawing.
     * TAB_BAR_HEIGHT is row 1 (row 0 is the tab bar).
     * We draw the header with A_BOLD then add the border '|' character.
     *
     * printw("%-*s", width, text) left-justifies `text` in a field of `width`
     * characters, padding with spaces on the right.  This ensures the header
     * fills the entire panel width (minus the border character).
     */
    move(TAB_BAR_HEIGHT, 0);
    attron(A_BOLD);
    printw("%-*s", FILETREE_WIDTH - 1, " Files");
    attroff(A_BOLD);
    addch('|');  /* vertical border separator between panel and editor */

    /* ---- Entry rows ---- */
    for (int r = 0; r < entry_rows; r++) {
        int screen_row = TAB_BAR_HEIGHT + 1 + r;  /* +1 for header */
        int idx        = ed->filetree_scroll + r;

        move(screen_row, 0);

        if (idx >= ft->count) {
            /*
             * We have scrolled past the last entry.  Draw a blank row to
             * fill the panel height and keep the border '|' consistent.
             */
            printw("%-*s", FILETREE_WIDTH - 1, "");
            addch('|');
            continue;
        }

        FlatEntry *e          = &ft->entries[idx];
        int        is_cursor  = (idx == ed->filetree_cursor);

        /*
         * Calculate the indentation for this entry.
         * Each nesting level adds 2 spaces.  We cap indent so there is always
         * at least 3 characters left for the icon, space, and name.
         */
        int indent = e->depth * 2;
        if (indent >= FILETREE_WIDTH - 3)
            indent = FILETREE_WIDTH - 3;

        /*
         * Icon character:
         *   '>' — collapsed directory (has children not shown)
         *   'v' — expanded directory (children are shown below it)
         *   ' ' — regular file
         */
        char icon = ' ';
        if (e->is_dir) {
            icon = filetree_is_expanded(ft, e->path) ? 'v' : '>';
        }

        /*
         * How many characters are available for the filename?
         *   FILETREE_WIDTH - 1   : full panel minus the border '|'
         *   - indent             : minus indentation
         *   - 2                  : minus icon + space after icon
         */
        int name_width = FILETREE_WIDTH - 1 - indent - 2;
        if (name_width < 1) name_width = 1;

        /*
         * Build the display string for this entry into a local buffer.
         *
         * snprintf format: "%*s%c %-*.*s"
         *   %*s           — `indent` spaces (empty string padded to width)
         *   %c            — the icon character
         *   (space)       — one space between icon and name
         *   %-*.*s        — name, left-justified, min and max width = name_width
         *                   (the '.' before * means "max chars to print" —
         *                    this truncates long names instead of wrapping)
         */
        char line_buf[FILETREE_WIDTH + 1];
        snprintf(line_buf, sizeof(line_buf), "%*s%c %-*.*s",
                 indent, "",
                 icon,
                 name_width, name_width, e->name);

        /*
         * Apply the appropriate visual attribute for this row, then print it.
         *
         * Priority (highest first):
         *   1. Cursor + focus   : CPAIR_FILETREE_CURSOR (black on white) + bold
         *   2. Cursor, no focus : A_BOLD only (subtle indication)
         *   3. Directory        : CPAIR_FILETREE_DIR (blue) + bold
         *   4. Regular file     : no special attribute
         */
        if (is_cursor && ed->filetree_focus) {
            attron(COLOR_PAIR(CPAIR_FILETREE_CURSOR) | A_BOLD);
            printw("%-*s", FILETREE_WIDTH - 1, line_buf);
            attroff(COLOR_PAIR(CPAIR_FILETREE_CURSOR) | A_BOLD);
        } else if (is_cursor) {
            attron(A_BOLD);
            printw("%-*s", FILETREE_WIDTH - 1, line_buf);
            attroff(A_BOLD);
        } else if (e->is_dir) {
            attron(COLOR_PAIR(CPAIR_FILETREE_DIR) | A_BOLD);
            printw("%-*s", FILETREE_WIDTH - 1, line_buf);
            attroff(COLOR_PAIR(CPAIR_FILETREE_DIR) | A_BOLD);
        } else {
            /* Plain file — use default terminal colors */
            printw("%-*s", FILETREE_WIDTH - 1, line_buf);
        }

        addch('|');  /* border character — always at column FILETREE_WIDTH-1 */
    }
}

/*
 * draw_region_hborder — draw a horizontal border line for the region box.
 *
 * Draws ┌──────┐ (top=1) or └──────┘ (top=0) across the given width
 * using ACS box-drawing characters in the CPAIR_REGION color (red).
 *
 * abs_row:   absolute terminal row (not relative to pane)
 * left_x:    leftmost column (absolute)
 * width:     total width of the border line
 * top:       1 = top border (┌─┐), 0 = bottom border (└─┘)
 */
static void draw_region_hborder(int abs_row, int left_x, int width, int top)
{
    chtype left_ch  = top ? ACS_ULCORNER : ACS_LLCORNER;
    chtype right_ch = top ? ACS_URCORNER : ACS_LRCORNER;

    attron(COLOR_PAIR(CPAIR_REGION));
    mvaddch(abs_row, left_x, left_ch);
    mvhline(abs_row, left_x + 1, ACS_HLINE, width - 2);
    mvaddch(abs_row, left_x + width - 1, right_ch);
    attroff(COLOR_PAIR(CPAIR_REGION));
}

/*
 * fill_to_edge — fill from the current cursor position to `right_col`
 * with spaces, using the current attribute.
 *
 * This replaces clrtoeol() for panes that don't extend to the terminal's
 * right edge.  clrtoeol() would clear INTO an adjacent pane — this function
 * only fills within the pane's boundaries.
 */
static void fill_to_edge(int right_col)
{
    int cx;
    /* getyx writes both row and col; we only need col (cx) */
    { int _cy; getyx(stdscr, _cy, cx); (void)_cy; }
    int n = right_col - cx;
    if (n > 0)
        hline(' ', n);      /* draw n spaces, does NOT advance cursor */
}

/*
 * draw_single_pane — render one pane's content within its screen rectangle.
 *
 * ed->active_pane must point to the pane being rendered (the caller sets
 * this temporarily before calling).  All per-pane state (cursor, viewport,
 * selection, region) is read through ed->active_pane.
 *
 * The pane's screen rectangle (x, y, width, height) determines where on
 * the terminal the content is drawn.
 */
static void draw_single_pane(struct Editor *ed)
{
    Buffer *buf = editor_current_buffer(ed);

    /*
     * Pane dimensions — read from the active pane's screen rectangle.
     * These replace the old panel_w / text_rows / text_cols calculations
     * that assumed a single editor area filling the whole screen.
     *
     * pane_x:      leftmost column of this pane on the terminal
     * pane_y:      topmost row of this pane on the terminal
     * pane_w:      total width of this pane in columns
     * text_rows:   number of rows available for text content
     * text_cols:   usable columns for text (pane width minus gutter)
     * right_edge:  rightmost column of this pane (inclusive)
     */
    Pane *pane = ed->active_pane;
    int pane_x     = pane->x;
    int pane_y     = pane->y;
    int pane_w     = pane->width;
    int text_rows  = pane->height;
    int text_cols  = pane_w - GUTTER_WIDTH;
    int right_edge = pane_x + pane_w - 1;

    /*
     * Pre-compute selection bounds once, outside the row loop.
     */
    int sel_active = ed->active_pane->sel_active;
    int sel_sr = 0, sel_sc = 0, sel_er = 0, sel_ec = 0;

    if (sel_active) {
        int ar = ed->active_pane->sel_anchor_row, ac = ed->active_pane->sel_anchor_col;
        int cr = ed->active_pane->cursor_row,     cc = ed->active_pane->cursor_col;

        if (ar < cr || (ar == cr && ac <= cc)) {
            sel_sr = ar; sel_sc = ac;
            sel_er = cr; sel_ec = cc;
        } else {
            sel_sr = cr; sel_sc = cc;
            sel_er = ar; sel_ec = ac;
        }
    }

    /*
     * Bracket match — find the bracket that pairs with the one under the
     * cursor (if any).  We compute this once here so the render loop can
     * highlight both brackets without re-scanning the buffer per line.
     *
     * bm_row / bm_col are set to -1 when there is no active bracket match.
     */
    int bm_row = -1, bm_col = -1;
    editor_find_bracket_match(ed, &bm_row, &bm_col);

    /* ---- Syntax highlighting setup --------------------------------------- */

    /*
     * Detect which language this file is written in, based on its filename
     * extension.  LANG_NONE means "no highlighting" — syntax_highlight_line()
     * will simply set all tokens to SYN_NORMAL when called with LANG_NONE.
     */
    SyntaxLang syn_lang = syntax_detect_language(buf->filename);

    /*
     * Multi-line state: some constructs (C block comments, Python triple-
     * quoted strings) can span multiple source lines.  We need to know the
     * parse state at the TOP of the viewport, not just at line 0.
     *
     * We compute it by scanning every line from 0 up to view_row, carrying
     * the state forward.  For most files this is very fast (microseconds),
     * because we are just scanning characters — no memory allocation.
     *
     * A temporary tokens array is required by the API but its contents are
     * discarded here; we only care about the returned state integer.
     */
    int syn_ml_state = 0;
    if (syn_lang != LANG_NONE) {
        SyntaxToken syn_tmp[SYNTAX_MAX_LINE];
        for (int r = 0; r < ed->active_pane->view_row && r < buf->num_lines; r++) {
            syn_ml_state = syntax_highlight_line(syn_lang,
                                                  buffer_get_line(buf, r),
                                                  buffer_line_len(buf, r),
                                                  syn_tmp, syn_ml_state);
        }
    }

    if (ed->word_wrap) {
        /*
         * ================================================================
         * Word-wrap rendering path
         * ================================================================
         *
         * In word-wrap mode a single buffer line may occupy multiple screen
         * rows.  We iterate screen rows, tracking (buf_row, segment) where
         * `segment` is which chunk of the wrapped line we are drawing.
         *
         *   segment 0: columns [0             .. text_cols)
         *   segment 1: columns [text_cols     .. 2*text_cols)
         *   segment k: columns [k*text_cols   .. (k+1)*text_cols)
         */
        int buf_row = ed->active_pane->view_row;
        int segment = 0;           /* which wrapped segment of buf_row */

        /*
         * syn_line_tokens — token array for the current buffer line.
         * Declared outside the loop so it persists across screen rows that
         * belong to the same (wrapped) buffer line.
         *
         * syn_tokens — pointer into syn_line_tokens, or NULL when no
         * language is detected.  Passed directly to draw_line_with_search().
         */
        SyntaxToken syn_line_tokens[SYNTAX_MAX_LINE];
        const SyntaxToken *syn_tokens = NULL;

        int screen_row = 0;
        while (screen_row < text_rows) {
            /*
             * ---- Region top border ----
             * Before the first segment of the region start row, draw a
             * horizontal line ┌──────────┐ across the full editor width.
             */
            if (ed->active_pane->region_active && segment == 0
                    && buf_row == ed->active_pane->region_start_row) {
                draw_region_hborder(pane_y + screen_row, pane_x, pane_w, 1);
                screen_row++;
                if (screen_row >= text_rows) break;
            }

            /*
             * Move to the start of this screen row, offset right by panel_w
             * so we start drawing after the file-tree panel (if visible).
             */
            move(pane_y + screen_row, pane_x);

            if (buf_row >= buf->num_lines) {
                /* Past end of file */
                syn_tokens = NULL;
                attron(COLOR_PAIR(CPAIR_GUTTER));
                printw("  ~  ");
                attroff(COLOR_PAIR(CPAIR_GUTTER));
                fill_to_edge(right_edge + 1);
                screen_row++;
                continue;
            }

            /*
             * Tokenize at the start of each new buffer line (segment == 0).
             * Continuation segments (segment > 0) reuse the same token array
             * since they show characters from the same buffer line.
             */
            if (segment == 0 && syn_lang != LANG_NONE) {
                syn_ml_state = syntax_highlight_line(syn_lang,
                                                      buffer_get_line(buf, buf_row),
                                                      buffer_line_len(buf, buf_row),
                                                      syn_line_tokens,
                                                      syn_ml_state);
                syn_tokens = syn_line_tokens;
            } else if (segment == 0) {
                syn_tokens = NULL;
            }

            int is_cursor_row = (buf_row == ed->active_pane->cursor_row);
            int row_attr      = A_NORMAL;  /* no full-line highlight */

            /*
             * in_region — true if this buffer row falls within the region
             * marked by the user with Ctrl+U.  Used to draw a red box border
             * in the gutter and at the right edge of each region row.
             */
            int in_region = ed->active_pane->region_active
                            && buf_row >= ed->active_pane->region_start_row
                            && buf_row <= ed->active_pane->region_end_row;

            /* ---- Gutter ---- */
            if (in_region) {
                /*
                 * Region gutter: draw a red vertical bar (│) followed by
                 * a 3-digit line number and a space (5 chars total = GUTTER_WIDTH).
                 *
                 * The corner characters (┌ └ ┐ ┘) are now on dedicated
                 * horizontal border rows drawn above/below the region, so
                 * all content rows use ACS_VLINE for the left border.
                 */
                attron(COLOR_PAIR(CPAIR_REGION));
                if (is_cursor_row) attron(A_BOLD);
                addch(ACS_VLINE);              /* │ — left border */
                if (segment == 0)
                    printw("%3d ", buf_row + 1);  /* line number (3 digits) */
                else
                    printw("    ");               /* blank continuation */
                if (is_cursor_row) attroff(A_BOLD);
                attroff(COLOR_PAIR(CPAIR_REGION));
            } else {
                attron(COLOR_PAIR(CPAIR_GUTTER));
                if (segment == 0) {
                    /*
                     * First segment of this line: show the line number.
                     * Bold on the cursor row for emphasis.
                     */
                    if (is_cursor_row) attron(A_BOLD);
                    printw("%4d ", buf_row + 1);
                    if (is_cursor_row) attroff(A_BOLD);
                } else {
                    /*
                     * Continuation segment: leave gutter blank (no line number)
                     * so the wrapped text looks like a continuation of the line.
                     */
                    printw("     ");
                }
                attroff(COLOR_PAIR(CPAIR_GUTTER));
            }

            /* ---- Compute the visible slice of this segment ---- */
            const char *line_text = buffer_get_line(buf, buf_row);
            int         line_len  = buffer_line_len(buf, buf_row);

            int start_col = segment * text_cols;
            int draw_len  = line_len - start_col;
            if (draw_len < 0)        draw_len = 0;
            if (draw_len > text_cols) draw_len = text_cols;

            const char *draw_start = line_text + start_col;

            /* ---- Render ---- */
            int search_active = (ed->search_query[0] != '\0');
            /*
             * Use per-character rendering when any per-character attribute
             * is active: search highlights, bracket match, or syntax colors.
             * The fast segment-based path is only used for plain unstyled text.
             */
            int need_perchar  = search_active
                                || (bm_col >= 0 && buf_row == bm_row)
                                || (bm_col >= 0 && buf_row == ed->active_pane->cursor_row)
                                || (syn_tokens != NULL);

            if (need_perchar && draw_len > 0) {
                draw_line_with_search(buf_row,
                                      line_text, line_len,
                                      draw_start, draw_len,
                                      start_col, row_attr,
                                      sel_active,
                                      sel_sr, sel_sc, sel_er, sel_ec,
                                      bm_row, bm_col,
                                      syn_tokens,
                                      ed);
                attrset(row_attr);
                fill_to_edge(right_edge + 1);
                attrset(A_NORMAL);
            } else {
                /*
                 * Compute selection range for this segment, clamped to
                 * [start_col, start_col + draw_len].
                 */
                int row_sel_start = 0, row_sel_end = 0;
                if (sel_active && buf_row >= sel_sr && buf_row <= sel_er) {
                    row_sel_start = (buf_row == sel_sr) ? sel_sc : 0;
                    row_sel_end   = (buf_row == sel_er) ? sel_ec : line_len + 1;
                }

                /* Convert to coords relative to this segment */
                int vis_sel_start = row_sel_start - start_col;
                int vis_sel_end   = row_sel_end   - start_col;
                if (vis_sel_start < 0)        vis_sel_start = 0;
                if (vis_sel_end   < 0)        vis_sel_end   = 0;
                if (vis_sel_start > draw_len) vis_sel_start = draw_len;
                if (vis_sel_end   > draw_len) vis_sel_end   = draw_len;

                int has_sel = sel_active
                              && (row_sel_start != row_sel_end)
                              && (vis_sel_start < vis_sel_end);

                if (!has_sel) {
                    attrset(row_attr);
                    if (draw_len > 0)
                        draw_text_segment(draw_start, draw_len,
                                          ed->show_whitespace);
                    fill_to_edge(right_edge + 1);
                    attrset(A_NORMAL);
                } else {
                    if (vis_sel_start > 0) {
                        attrset(row_attr);
                        draw_text_segment(draw_start, vis_sel_start,
                                          ed->show_whitespace);
                    }
                    attrset(A_NORMAL);
                    attron(COLOR_PAIR(CPAIR_SELECTION));
                    draw_text_segment(draw_start + vis_sel_start,
                                      vis_sel_end - vis_sel_start,
                                      ed->show_whitespace);
                    attroff(COLOR_PAIR(CPAIR_SELECTION));
                    if (vis_sel_end < draw_len) {
                        attrset(row_attr);
                        draw_text_segment(draw_start + vis_sel_end,
                                          draw_len - vis_sel_end,
                                          ed->show_whitespace);
                    }
                    attrset(row_attr);
                    fill_to_edge(right_edge + 1);
                    attrset(A_NORMAL);
                }
            }

            /* ---- Region right border ---- */
            if (in_region) {
                /*
                 * Draw a red vertical bar (│) at the rightmost terminal column
                 * for every screen row that belongs to a region line.
                 *
                 * Corners are on the dedicated horizontal border rows, so
                 * all content rows use ACS_VLINE for the right border.
                 */
                attron(COLOR_PAIR(CPAIR_REGION));
                mvaddch(pane_y + screen_row, right_edge, ACS_VLINE);
                attroff(COLOR_PAIR(CPAIR_REGION));
            }

            /* ---- Advance (buf_row, segment) ---- */
            int was_region_end = (ed->active_pane->region_active
                                  && buf_row == ed->active_pane->region_end_row);
            segment++;
            int finished_line = (start_col + text_cols >= line_len);
            if (finished_line) {
                /* We have rendered the last (or only) segment of this line */
                buf_row++;
                segment = 0;
            }
            screen_row++;

            /*
             * ---- Region bottom border ----
             * After the last segment of the region end row, draw a
             * horizontal line └──────────┘ across the full editor width.
             */
            if (was_region_end && finished_line && screen_row < text_rows) {
                draw_region_hborder(pane_y + screen_row, pane_x, pane_w, 0);
                screen_row++;
            }
        }

    } else {
        /*
         * ================================================================
         * Normal (no wrap) rendering path — original logic
         * ================================================================
         */

        /*
         * syn_line_tokens — reused each iteration to hold per-character
         * token types for the current line.  Declared outside the loop so
         * the same 1 KB stack space is reused, not freshly allocated each
         * iteration.
         */
        SyntaxToken syn_line_tokens[SYNTAX_MAX_LINE];

        int buf_row = ed->active_pane->view_row;
        int screen_row = 0;
        while (screen_row < text_rows) {
            /*
             * ---- Region top border ----
             * Before the region start row, draw a horizontal line
             * ┌──────────┐ across the full editor width.
             */
            if (ed->active_pane->region_active && buf_row == ed->active_pane->region_start_row) {
                draw_region_hborder(pane_y + screen_row, pane_x, pane_w, 1);
                screen_row++;
                if (screen_row >= text_rows) break;
            }

            /*
             * Position at the start of this screen row, offset right by panel_w
             * so we draw in the editor area (right of the file-tree panel).
             */
            move(pane_y + screen_row, pane_x);

            if (buf_row >= buf->num_lines) {
                /*
                 * Past end of file — draw a tilde in the gutter (like vim does)
                 * to indicate "no line here".
                 */
                attron(COLOR_PAIR(CPAIR_GUTTER));
                printw("  ~  ");
                attroff(COLOR_PAIR(CPAIR_GUTTER));
                fill_to_edge(right_edge + 1);
                screen_row++;
                buf_row++;
                continue;
            }

            /*
             * Tokenize this line for syntax highlighting.
             * syn_ml_state carries the multi-line parse state (e.g. "inside
             * a block comment") from the previous line into this one.
             * We update it in-place so it flows correctly down the screen.
             */
            const SyntaxToken *syn_tokens = NULL;
            if (syn_lang != LANG_NONE) {
                syn_ml_state = syntax_highlight_line(syn_lang,
                                                      buffer_get_line(buf, buf_row),
                                                      buffer_line_len(buf, buf_row),
                                                      syn_line_tokens,
                                                      syn_ml_state);
                syn_tokens = syn_line_tokens;
            }

            /* ---- Gutter: line number (or region border) --------------------- */
            int is_cursor_row = (buf_row == ed->active_pane->cursor_row);

            /*
             * in_region — true if this row falls inside the user's marked region.
             */
            int in_region = ed->active_pane->region_active
                            && buf_row >= ed->active_pane->region_start_row
                            && buf_row <= ed->active_pane->region_end_row;

            if (in_region) {
                /*
                 * Region gutter: draw a red vertical bar (│) followed by
                 * a 3-digit line number and a space (5 chars total = GUTTER_WIDTH).
                 *
                 * Corners are on the dedicated horizontal border rows, so
                 * all content rows use ACS_VLINE for the left border.
                 */
                attron(COLOR_PAIR(CPAIR_REGION));
                if (is_cursor_row) attron(A_BOLD);
                addch(ACS_VLINE);              /* │ — left border */
                printw("%3d ", buf_row + 1);
                if (is_cursor_row) attroff(A_BOLD);
                attroff(COLOR_PAIR(CPAIR_REGION));
            } else {
                attron(COLOR_PAIR(CPAIR_GUTTER));
                if (is_cursor_row) attron(A_BOLD);
                printw("%4d ", buf_row + 1);
                if (is_cursor_row) attroff(A_BOLD);
                attroff(COLOR_PAIR(CPAIR_GUTTER));
            }

            /* ---- Compute what portion of this row (if any) is selected ------- */

            /*
             * row_sel_start and row_sel_end are column indices (in the buffer,
             * not on screen) of the selected range on this row.
             *
             * row_sel_start == row_sel_end means nothing is selected on this row.
             */
            int row_sel_start = 0, row_sel_end = 0;

            if (sel_active && buf_row >= sel_sr && buf_row <= sel_er) {
                int line_len = buffer_line_len(buf, buf_row);

                row_sel_start = (buf_row == sel_sr) ? sel_sc : 0;

                if (buf_row == sel_er) {
                    row_sel_end = sel_ec;
                } else {
                    /*
                     * Middle rows of a multi-line selection: the entire line is
                     * selected.  We extend the highlight one past the last char
                     * so the selection visually covers the newline position too.
                     */
                    row_sel_end = line_len + 1;
                }
            }

            /* ---- Set up the base attribute for unselected text on this row --- */

            /*
             * All rows use A_NORMAL (terminal defaults).
             * Selected segments always use CPAIR_SELECTION regardless of row.
             */
            int row_attr = A_NORMAL;  /* no full-line highlight */

            /* ---- Compute what slice of the line is visible on screen --------- */

            const char *line_text = buffer_get_line(buf, buf_row);
            int         line_len  = buffer_line_len(buf, buf_row);
            int         view_col  = ed->active_pane->view_col;

            /* Clamp view_col so we don't go past end of line */
            int start_col = (view_col <= line_len) ? view_col : line_len;
            int draw_len  = line_len - start_col;
            if (draw_len > text_cols) draw_len = text_cols;

            /*
             * draw_start points to the first character that will be rendered.
             */
            const char *draw_start = line_text + start_col;

            /* ---- Render the line in up to three segments --------------------- */

            /*
             * Convert the selection range from buffer coords to screen coords
             * (relative to the start of the visible text area).
             * Clamp to [0, draw_len] so we never draw outside the visible area.
             */
            int vis_sel_start = row_sel_start - view_col;
            int vis_sel_end   = row_sel_end   - view_col;

            if (vis_sel_start < 0)         vis_sel_start = 0;
            if (vis_sel_end   < 0)         vis_sel_end   = 0;
            if (vis_sel_start > draw_len)  vis_sel_start = draw_len;
            if (vis_sel_end   > draw_len)  vis_sel_end   = draw_len;

            int has_selection_on_row = sel_active
                                       && (row_sel_start != row_sel_end)
                                       && (vis_sel_start < vis_sel_end);

            int search_active2 = (ed->search_query[0] != '\0');
            int need_perchar2  = search_active2
                                 || (bm_col >= 0 && buf_row == bm_row)
                                 || (bm_col >= 0 && buf_row == ed->active_pane->cursor_row)
                                 || (syn_tokens != NULL);

            if (need_perchar2) {
                /*
                 * Per-character rendering: used when search, bracket match,
                 * or syntax highlighting is active so individual characters
                 * can receive different colors.
                 */
                if (draw_len > 0) {
                    draw_line_with_search(buf_row,
                                          line_text, line_len,
                                          draw_start, draw_len,
                                          start_col, row_attr,
                                          sel_active,
                                          sel_sr, sel_sc, sel_er, sel_ec,
                                          bm_row, bm_col,
                                          syn_tokens,
                                          ed);
                }
                attrset(row_attr);
                fill_to_edge(right_edge + 1);
                attrset(A_NORMAL);

            } else if (!has_selection_on_row) {
                /* No search, no selection — render the whole line with row_attr */
                attrset(row_attr);
                if (draw_len > 0)
                    draw_text_segment(draw_start, draw_len, ed->show_whitespace);
                fill_to_edge(right_edge + 1);
                attrset(A_NORMAL);
            } else {
                /*
                 * No search but selection present: split into up to three segments.
                 *
                 *   [0 .. vis_sel_start)          row_attr
                 *   [vis_sel_start .. vis_sel_end) CPAIR_SELECTION
                 *   [vis_sel_end .. draw_len)      row_attr
                 */

                if (vis_sel_start > 0) {
                    attrset(row_attr);
                    draw_text_segment(draw_start, vis_sel_start,
                                      ed->show_whitespace);
                }

                attrset(A_NORMAL);
                attron(COLOR_PAIR(CPAIR_SELECTION));
                if (vis_sel_end > vis_sel_start)
                    draw_text_segment(draw_start + vis_sel_start,
                                      vis_sel_end - vis_sel_start,
                                      ed->show_whitespace);
                attroff(COLOR_PAIR(CPAIR_SELECTION));

                if (vis_sel_end < draw_len) {
                    attrset(row_attr);
                    draw_text_segment(draw_start + vis_sel_end,
                                      draw_len - vis_sel_end,
                                      ed->show_whitespace);
                }

                attrset(row_attr);
                fill_to_edge(right_edge + 1);
                attrset(A_NORMAL);
            }

            /* ---- Region right border ---- */
            if (in_region) {
                /*
                 * Draw a red vertical bar (│) at the rightmost terminal column.
                 *
                 * Corners are on the dedicated horizontal border rows, so
                 * all content rows use ACS_VLINE for the right border.
                 */
                attron(COLOR_PAIR(CPAIR_REGION));
                mvaddch(pane_y + screen_row, right_edge, ACS_VLINE);
                attroff(COLOR_PAIR(CPAIR_REGION));
            }

            /* ---- Advance ---- */
            int was_region_end = (ed->active_pane->region_active
                                  && buf_row == ed->active_pane->region_end_row);
            buf_row++;
            screen_row++;

            /*
             * ---- Region bottom border ----
             * After the region end row, draw a horizontal line
             * └──────────┘ across the full editor width.
             */
            if (was_region_end && screen_row < text_rows) {
                draw_region_hborder(pane_y + screen_row, pane_x, pane_w, 0);
                screen_row++;
            }
        }
    }
}

/* ============================================================================
 * Internal: draw separator lines between split panes
 * ============================================================================ */

/*
 * draw_pane_separators — recursively draw the separator lines for the tree.
 *
 * Each internal node in the pane tree represents a split.  The separator
 * occupies the 1-row or 1-column gap between the two children's rectangles.
 *
 * Horizontal split:  draw a horizontal line of ACS_HLINE characters.
 * Vertical split:    draw a vertical line of ACS_VLINE characters.
 *
 * The separator position is derived from the children's rectangles:
 *   - Horizontal: row = child1.y + child1.height (the row between them)
 *   - Vertical:   col = child1.x + child1.width (the column between them)
 */
static void draw_pane_separators(PaneNode *node)
{
    if (!node || node->split == SPLIT_NONE) return;

    if (node->split == SPLIT_HORIZONTAL) {
        /*
         * The separator row is between child1's bottom and child2's top.
         * child1 occupies rows [y .. y+height), so the separator is at
         * y + height = child2->y - 1 (they should match).
         */
        Pane *leaves1[PANE_MAX];
        int count1 = 0;
        pane_collect_leaves(node->child1, leaves1, &count1);
        if (count1 > 0) {
            /* Use the first child1 leaf to find the separator row */
            Pane *c1 = leaves1[0];
            int sep_row = c1->y + c1->height;
            int left_x  = c1->x;
            /* Find the width by looking at the second child too */
            Pane *leaves2[PANE_MAX];
            int count2 = 0;
            pane_collect_leaves(node->child2, leaves2, &count2);
            int width = (count2 > 0) ? leaves2[0]->width : c1->width;

            attron(COLOR_PAIR(CPAIR_PANE_BORDER));
            mvhline(sep_row, left_x, ACS_HLINE, width);
            attroff(COLOR_PAIR(CPAIR_PANE_BORDER));
        }
    } else {
        /* Vertical split: separator column between left and right children */
        Pane *leaves1[PANE_MAX];
        int count1 = 0;
        pane_collect_leaves(node->child1, leaves1, &count1);
        if (count1 > 0) {
            Pane *c1 = leaves1[0];
            int sep_col = c1->x + c1->width;
            int top_y   = c1->y;
            int height  = c1->height;

            attron(COLOR_PAIR(CPAIR_PANE_BORDER));
            mvvline(sep_col, top_y, ACS_VLINE, height);
            attroff(COLOR_PAIR(CPAIR_PANE_BORDER));
        }
    }

    /* Recurse into children for nested splits */
    draw_pane_separators(node->child1);
    draw_pane_separators(node->child2);
}

/* ============================================================================
 * Internal: draw all editor panes (public entry point for rendering)
 * ============================================================================ */

static void draw_editor_area(struct Editor *ed)
{
    /*
     * Compute the screen rectangle available for panes.
     * The file tree panel (if visible) reduces the left side.
     * The tab bar occupies the top, and the status bar occupies the bottom.
     */
    int panel_w = (ed->show_filetree && ed->filetree) ? FILETREE_WIDTH : 0;
    int area_x  = panel_w;
    int area_y  = TAB_BAR_HEIGHT;
    int area_w  = ed->term_cols - panel_w;
    int area_h  = ed->term_rows - 1 - TAB_BAR_HEIGHT;

    /*
     * Assign screen rectangles to all leaf panes based on the split tree.
     * This must be called before rendering so each pane knows where to draw.
     */
    pane_layout(ed->pane_root, area_x, area_y, area_w, area_h);

    /*
     * Collect all leaf panes into a flat array for iteration.
     */
    Pane *leaves[PANE_MAX];
    int num_leaves = 0;
    pane_collect_leaves(ed->pane_root, leaves, &num_leaves);

    /*
     * Render each pane.  We temporarily set ed->active_pane to the pane
     * being rendered so that all existing code that reads from active_pane
     * (cursor, viewport, selection, etc.) automatically uses the right pane.
     *
     * This includes editor_current_buffer() and editor_find_bracket_match(),
     * which are called from within draw_single_pane().
     */
    Pane *saved_active = ed->active_pane;
    for (int i = 0; i < num_leaves; i++) {
        ed->active_pane = leaves[i];
        draw_single_pane(ed);
    }
    ed->active_pane = saved_active;

    /*
     * Draw separator lines between panes (if there are splits).
     */
    if (num_leaves > 1)
        draw_pane_separators(ed->pane_root);
}

/* ============================================================================
 * Internal: draw the status bar
 * ============================================================================ */

static void draw_status_bar(struct Editor *ed)
{
    Buffer *buf = editor_current_buffer(ed);

    int status_row = ed->term_rows - 1;

    /* Choose color based on whether the file has unsaved changes */
    int color = buf->dirty ? CPAIR_STATUS_DIRTY : CPAIR_STATUS;
    attron(COLOR_PAIR(color) | A_BOLD);

    move(status_row, 0);

    /* Left side: filename and modified indicator */
    char left[256];
    const char *name = buf->filename ? buf->filename : "[No Name]";
    const char *mod  = buf->dirty    ? " [+]"        : "";
    snprintf(left, sizeof(left), "  %.60s%s", name, mod);

    /* Right side: cursor position */
    char right[64];
    snprintf(right, sizeof(right), "Ln %d, Col %d  ",
             ed->active_pane->cursor_row + 1,
             ed->active_pane->cursor_col + 1);

    /* Print left section */
    printw("%-*s", ed->term_cols - (int)strlen(right), left);

    /* Print right section — move to the correct column first */
    move(status_row, ed->term_cols - (int)strlen(right));
    printw("%s", right);

    attroff(COLOR_PAIR(color) | A_BOLD);

    /* ---- Help hint (shown when no status message is set) -------------- */
    /*
     * We re-use a small portion of the status bar for a key-binding hint
     * when status_msg is empty.  This gives new users a clue about how to
     * save and quit.
     */
    if (ed->status_msg[0] == '\0') {
        /* Overwrite the centre of the status bar with hint text */
        const char *hint = "Ctrl+S save  Ctrl+Q quit";
        int hint_col = (ed->term_cols - (int)strlen(hint)) / 2;
        if (hint_col > (int)strlen(left) && hint_col > 0) {
            attron(COLOR_PAIR(color));
            mvprintw(status_row, hint_col, "%s", hint);
            attroff(COLOR_PAIR(color));
        }
    } else {
        /* Show the status message in the centre */
        int msg_col = (ed->term_cols - (int)strlen(ed->status_msg)) / 2;
        if (msg_col < 0) msg_col = 0;
        attron(COLOR_PAIR(color) | A_BOLD);
        mvprintw(status_row, msg_col, "%.80s", ed->status_msg);
        attroff(COLOR_PAIR(color) | A_BOLD);
    }
}

/* ============================================================================
 * display_render
 * ============================================================================ */

void display_render(struct Editor *ed)
{
    /*
     * erase() is like clear() but does not move the cursor and avoids
     * the full-screen flicker that clear() can cause.
     */
    erase();

    draw_tab_bar(ed);

    /*
     * Draw the file explorer panel BEFORE the editor area.
     * The panel occupies the leftmost FILETREE_WIDTH columns; draw_editor_area
     * will offset its own drawing rightward by panel_w to avoid overlapping.
     */
    if (ed->show_filetree && ed->filetree)
        draw_filetree_panel(ed);

    draw_editor_area(ed);
    draw_status_bar(ed);

    /*
     * Position the terminal cursor.
     *
     * There are two cases:
     *   A) Filetree has focus: put the cursor on the highlighted tree entry.
     *   B) Editor has focus:   put the cursor at the editor cursor position.
     *
     * Pane coordinates are used for editor cursor positioning.
     */
    int screen_col, screen_row;

    if (ed->filetree_focus && ed->show_filetree && ed->filetree) {
        /*
         * Case A: cursor belongs in the file-tree panel.
         *
         * The header occupies row TAB_BAR_HEIGHT.  Entry rows start at
         * TAB_BAR_HEIGHT + 1.  The visible entry at position (cursor - scroll)
         * relative to the scroll offset is shown at that offset.
         */
        int panel_entry_rows = ed->term_rows - TAB_BAR_HEIGHT - 2; /* -1 header, -1 status */
        int vis_row = ed->filetree_cursor - ed->filetree_scroll;
        if (vis_row < 0)               vis_row = 0;
        if (vis_row >= panel_entry_rows) vis_row = panel_entry_rows - 1;

        screen_row = TAB_BAR_HEIGHT + 1 + vis_row;  /* +1 to skip header */
        screen_col = 0;

    } else if (ed->word_wrap) {
        /*
         * Case B (word-wrap on): compute cursor row from wrapped-line heights.
         *
         * text_cols_wr uses the active pane's width (not the full terminal)
         * so wrapping matches what draw_single_pane actually rendered.
         */
        Pane *ap = ed->active_pane;
        int text_cols_wr = ap->width - GUTTER_WIDTH;
        if (text_cols_wr <= 0) text_cols_wr = 1;

        /* Count screen rows consumed by lines before cursor_row */
        int sr = 0;
        Buffer *buf_wr = editor_current_buffer(ed);
        if (buf_wr) {
            for (int r = ap->view_row; r < ap->cursor_row; r++) {
                int len = buffer_line_len(buf_wr, r);
                sr += line_screen_rows_d(len, text_cols_wr);
            }
        }
        /* Add the sub-row within the cursor line */
        sr += ap->cursor_col / text_cols_wr;

        screen_row = ap->y + sr;
        screen_col = ap->x + GUTTER_WIDTH + (ap->cursor_col % text_cols_wr);

        /*
         * Adjust for region border rows that consume screen space.
         */
        if (ap->region_active) {
            if (ap->cursor_row >= ap->region_start_row)
                screen_row++;
            if (ap->cursor_row > ap->region_end_row)
                screen_row++;
        }

    } else {
        /*
         * Case B (normal mode): straightforward linear mapping.
         * Use the active pane's rectangle for positioning.
         */
        Pane *ap = ed->active_pane;
        screen_col = ap->x + GUTTER_WIDTH + (ap->cursor_col - ap->view_col);
        screen_row = ap->y + (ap->cursor_row - ap->view_row);

        /* Adjust for region border rows */
        if (ap->region_active) {
            if (ap->cursor_row >= ap->region_start_row)
                screen_row++;
            if (ap->cursor_row > ap->region_end_row)
                screen_row++;
        }
    }

    /* Clamp to prevent the cursor from escaping the text area */
    Pane *ap_clamp = ed->active_pane;
    int text_area_top    = ap_clamp->y;
    int text_area_bottom = ap_clamp->y + ap_clamp->height - 1;
    int min_col          = (ed->filetree_focus && ed->show_filetree && ed->filetree)
                           ? 0
                           : ap_clamp->x + GUTTER_WIDTH;
    int max_col          = ap_clamp->x + ap_clamp->width - 1;

    if (screen_row < text_area_top)    screen_row = text_area_top;
    if (screen_row > text_area_bottom) screen_row = text_area_bottom;
    if (screen_col < min_col)          screen_col = min_col;
    if (screen_col > max_col)          screen_col = max_col;

    move(screen_row, screen_col);

    /*
     * refresh() sends the accumulated changes to the actual terminal.
     * Nothing is visible on screen until refresh() is called.
     */
    refresh();
}

/* ============================================================================
 * display_prompt
 * ============================================================================ */

char *display_prompt(struct Editor *ed, const char *prompt)
{
    /*
     * A small fixed buffer for the user's input.
     * 255 chars is plenty for a file path.
     */
    char input[256];
    int  len = 0;
    input[0] = '\0';

    int status_row = ed->term_rows - 1;

    for (;;) {
        /*
         * Redraw the status bar with the prompt and current input.
         * We clear the whole row first by printing spaces, then overwrite
         * with the prompt text.
         */
        attron(COLOR_PAIR(CPAIR_STATUS) | A_BOLD);
        move(status_row, 0);
        printw("%-*s", ed->term_cols, "");        /* blank the row */
        mvprintw(status_row, 0, "%s%s", prompt, input);
        attroff(COLOR_PAIR(CPAIR_STATUS) | A_BOLD);

        /* Place the terminal cursor right after the typed text */
        move(status_row, (int)strlen(prompt) + len);
        refresh();

        int key = getch();

        if (key == '\r' || key == '\n' || key == KEY_ENTER) {
            /* User pressed Enter — return what they typed */
            return strdup(input);

        } else if (key == 27) {
            /* Escape — cancel the prompt */
            return NULL;

        } else if ((key == KEY_BACKSPACE || key == 127 || key == '\b')
                   && len > 0) {
            /* Backspace — delete the last character */
            input[--len] = '\0';

        } else if (key >= 0x20 && key <= 0x7e
                   && len < (int)sizeof(input) - 1) {
            /* Printable character — append it */
            input[len++] = (char)key;
            input[len]   = '\0';
        }
    }
}
