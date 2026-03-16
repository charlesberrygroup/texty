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
#include "git.h"        /* for GitLineStatus, GitState */
#include "build.h"      /* for BuildResult, BuildError */
#include "finder.h"     /* for FinderFile, FinderResult, finder_filter, etc. */
#include "theme.h"      /* for Theme, ThemeColorDef */

#include <ncurses.h>
#include <stdlib.h>    /* for malloc, free — used by display_finder_popup */
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
     * ESCDELAY controls how long ncurses waits after receiving an ESC byte
     * (0x1B) to determine whether it's a standalone Escape keypress or
     * the start of a multi-byte escape sequence (like an arrow key).
     *
     * The default is often 1000ms, which makes Escape feel sluggish.
     * 25ms is fast enough to feel instant but long enough for terminal
     * escape sequences to arrive (they typically take < 5ms).
     */
    set_escdelay(25);

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

        /* Git gutter markers */
        init_pair(CPAIR_GIT_ADDED,    COLOR_GREEN,  -1);
        init_pair(CPAIR_GIT_MODIFIED, COLOR_YELLOW, -1);
        init_pair(CPAIR_GIT_DELETED,  COLOR_RED,    -1);

        /* Git status panel */
        init_pair(CPAIR_GIT_PANEL_CURSOR, COLOR_BLACK, COLOR_WHITE);

        /* Inline diff: phantom (old/deleted) lines shown in red */
        init_pair(CPAIR_GIT_OLD_LINE, COLOR_RED, -1);

        /* Blame annotations: dim cyan on default background */
        init_pair(CPAIR_BLAME, COLOR_CYAN, -1);

        /* Build panel */
        init_pair(CPAIR_BUILD_ERROR,   COLOR_RED,   -1);
        init_pair(CPAIR_BUILD_WARNING, COLOR_YELLOW, -1);
        init_pair(CPAIR_BUILD_CURSOR,  COLOR_BLACK, COLOR_WHITE);
        init_pair(CPAIR_BUILD_SUCCESS, COLOR_GREEN,  -1);

        /* Fuzzy finder popup */
        init_pair(CPAIR_FINDER_BORDER, COLOR_CYAN,  -1);
        init_pair(CPAIR_FINDER_CURSOR, COLOR_BLACK, COLOR_WHITE);
        init_pair(CPAIR_FINDER_MATCH,  COLOR_GREEN, -1);

        /* LSP diagnostics */
        init_pair(CPAIR_LSP_ERROR,   COLOR_RED,    -1);
        init_pair(CPAIR_LSP_WARNING, COLOR_YELLOW, -1);

        /* Staged files in the git status panel: green (matches git CLI) */
        init_pair(CPAIR_GIT_STAGED, COLOR_GREEN, -1);
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
 * display_apply_theme — set ncurses color pairs from a Theme struct
 * ============================================================================ */

void display_apply_theme(const void *theme_ptr)
{
    /*
     * theme_ptr is a const Theme* passed as void* to avoid including theme.h
     * in display.h (which would create a circular dependency).
     */
    const Theme *t = (const Theme *)theme_ptr;
    if (!t) return;

    /*
     * Set the terminal's default foreground and background colors.
     *
     * assume_default_colors(fg, bg) tells ncurses what the terminal's
     * "default" colors are.  This affects:
     *   - Color pair 0 (the default pair used by A_NORMAL)
     *   - Any color pair that uses -1 (terminal default) for fg or bg
     *
     * For example, if default_bg = BLACK, then all text rendered with
     * the default background (-1) will appear on a black background,
     * even if the user's terminal is set to a light background.
     *
     * The value -1 means "keep whatever the terminal already has" —
     * so the Default Dark theme (-1, -1) respects the user's terminal
     * settings, while Gruvbox/Monokai force a dark background.
     */
    assume_default_colors(t->default_fg, t->default_bg);

    /*
     * Re-register every color pair.  init_pair() can be called at any time
     * after start_color(), and the new colors take effect on the next refresh.
     *
     * Skip CPAIR_DEFAULT (1) and CPAIR_CURLINE (5) — these are intentionally
     * NOT registered with init_pair() due to a macOS ncurses bug where -1
     * (terminal default) is misinterpreted as COLOR_BLACK.  Normal text uses
     * attrset(A_NORMAL) and cursor line uses A_REVERSE instead.
     */
    for (int i = 2; i <= THEME_NUM_PAIRS; i++) {
        if (i == CPAIR_CURLINE) continue;  /* skip — uses A_REVERSE */
        init_pair(i, t->pairs[i].fg, t->pairs[i].bg);
    }

    /*
     * Force a full screen redraw so the new background color is visible
     * everywhere, not just where text is drawn.
     */
    bkgd(COLOR_PAIR(0));
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
        int         active = (i == ed->current_buffer);

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
 *   The cursor bracket itself is at (ed->cursor_row, ed->cursor_col).
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
    int         match_row  = ed->search_match_row;
    int         match_col  = ed->search_match_col;

    for (int i = 0; i < draw_len; i++) {
        int buf_col = view_col + i;

        int attr;

        if ((buf_row == ed->cursor_row && buf_col == ed->cursor_col
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
 * Draws ┌──────┐ (top=1) or └──────┘ (top=0) across the full editor width
 * using ACS box-drawing characters in the CPAIR_REGION color (red).
 *
 * screen_row:  0-based row within the text area (not absolute terminal row)
 * panel_w:     column offset for the file-tree panel (0 if hidden)
 * ed:          editor state, used for term_cols
 * top:         1 = top border (┌─┐), 0 = bottom border (└─┘)
 */
static void draw_region_hborder(int screen_row, int panel_w,
                                struct Editor *ed, int top)
{
    int row = TAB_BAR_HEIGHT + screen_row;
    chtype left_ch  = top ? ACS_ULCORNER : ACS_LLCORNER;
    chtype right_ch = top ? ACS_URCORNER : ACS_LRCORNER;
    int width = ed->term_cols - panel_w;  /* total columns in editor area */

    attron(COLOR_PAIR(CPAIR_REGION));
    mvaddch(row, panel_w, left_ch);
    mvhline(row, panel_w + 1, ACS_HLINE, width - 2);
    mvaddch(row, ed->term_cols - 1, right_ch);
    attroff(COLOR_PAIR(CPAIR_REGION));
}

/*
 * draw_git_marker — draw the git gutter marker for a single buffer line.
 *
 * Replaces the trailing space in the line-number gutter with a colored
 * character when the line has changed relative to HEAD:
 *   - ADDED:    green '+'
 *   - MODIFIED: yellow '~'
 *   - DELETED:  red '_' (marks the line above a deletion point)
 *   - UNCHANGED or no git data: plain space ' '
 *
 * `gs` is the buffer's GitState.  `buf_row` is the 0-based line index.
 */
static void draw_git_marker(const GitState *gs, int buf_row)
{
    if (!gs || !gs->line_status || buf_row < 0 || buf_row >= gs->line_count) {
        addch(' ');   /* no git info — normal trailing space */
        return;
    }

    switch (gs->line_status[buf_row]) {
    case GIT_LINE_ADDED:
        attron(COLOR_PAIR(CPAIR_GIT_ADDED));
        addch('+');
        attroff(COLOR_PAIR(CPAIR_GIT_ADDED));
        break;
    case GIT_LINE_MODIFIED:
        attron(COLOR_PAIR(CPAIR_GIT_MODIFIED));
        addch('~');
        attroff(COLOR_PAIR(CPAIR_GIT_MODIFIED));
        break;
    case GIT_LINE_DELETED:
        attron(COLOR_PAIR(CPAIR_GIT_DELETED));
        addch('_');
        attroff(COLOR_PAIR(CPAIR_GIT_DELETED));
        break;
    default:
        addch(' ');   /* unchanged */
        break;
    }
}

/*
 * draw_lsp_line_indicator — draw an LSP diagnostic indicator after the line number.
 *
 * If the given buffer line has an LSP error, draws a red dot.
 * If it has a warning, draws a yellow dot.
 * If both, error takes priority.
 *
 * The indicator replaces the space between the line number and the text.
 * Returns 1 if an indicator was drawn, 0 if not.
 */
static int draw_lsp_line_indicator(const Buffer *buf, int buf_row)
{
    if (!buf || buf->lsp_diagnostics.count == 0) return 0;

    int worst_severity = 0;  /* 0 = none, 1 = error, 2 = warning */
    for (int i = 0; i < buf->lsp_diagnostics.count; i++) {
        if (buf->lsp_diagnostics.items[i].line == buf_row) {
            int sev = buf->lsp_diagnostics.items[i].severity;
            if (sev == LSP_SEV_ERROR && worst_severity != 1)
                worst_severity = 1;
            else if (sev == LSP_SEV_WARNING && worst_severity == 0)
                worst_severity = 2;
        }
    }

    if (worst_severity == 0) return 0;

    /* Draw a colored marker — overwrite the git marker column */
    if (worst_severity == 1) {
        attron(COLOR_PAIR(CPAIR_LSP_ERROR) | A_BOLD);
        printw("E");
        attroff(COLOR_PAIR(CPAIR_LSP_ERROR) | A_BOLD);
    } else {
        attron(COLOR_PAIR(CPAIR_LSP_WARNING));
        printw("W");
        attroff(COLOR_PAIR(CPAIR_LSP_WARNING));
    }
    return 1;
}

/*
 * draw_phantom_line — render one "old" line from HEAD in the inline diff view.
 *
 * Phantom lines are read-only lines from the HEAD version of the file that
 * were deleted or replaced.  They appear in red with a "  -  " gutter marker
 * so the user can see what was removed.
 *
 * `old_line`   — the text of the old line (from git diff).
 * `text_cols`  — number of columns available for text content.
 * `show_ws`    — whether to render spaces as visible dots.
 */
static void draw_phantom_line(const char *old_line, int text_cols,
                              int show_ws, int blame_w)
{
    /*
     * Gutter: "  -  " — 5 chars (GUTTER_WIDTH).
     * The '-' in the middle visually indicates this is a removed line,
     * similar to unified diff output.  Rendered in red.
     */
    attron(COLOR_PAIR(CPAIR_GIT_OLD_LINE));
    printw("  -  ");

    /* Blank space for the blame column (if blame is active) */
    if (blame_w > 0)
        printw("%*s", blame_w, "");

    /* Draw the old line text, truncated to the visible text width */
    if (old_line) {
        int len = (int)strlen(old_line);
        if (len > text_cols) len = text_cols;
        for (int i = 0; i < len; i++) {
            if (show_ws && old_line[i] == ' ')
                addch(ACS_BULLET);   /* visible dot for spaces */
            else
                addch(old_line[i]);
        }
    }
    clrtoeol();
    attroff(COLOR_PAIR(CPAIR_GIT_OLD_LINE));
}

/*
 * draw_blame_annotation — render the blame column for one buffer line.
 *
 * Draws a BLAME_WIDTH-column annotation showing "author    YYYY-MM-DD|"
 * in dim cyan.  When blame data is unavailable for a line (past EOF or
 * no data), draws blank spaces to keep columns aligned.
 *
 * `bd`      — blame data for the current buffer (may be NULL).
 * `buf_row` — 0-based buffer line index.
 */
static void draw_blame_annotation(const GitBlameData *bd, int buf_row)
{
    attron(COLOR_PAIR(CPAIR_BLAME));

    if (bd && bd->lines && buf_row >= 0 && buf_row < bd->count) {
        const GitBlameLine *bl = &bd->lines[buf_row];

        /*
         * Format: "%-10.10s %10s|"
         * Author name: left-aligned, truncated to 10 chars.
         * Date:        right-aligned, 10 chars ("YYYY-MM-DD" or spaces).
         * Separator:   '|' as visual boundary before text area.
         * Total:       10 + 1 + 10 + 1 + 1 = 23 = BLAME_WIDTH.
         */
        char buf[BLAME_WIDTH + 1];
        snprintf(buf, sizeof(buf), "%-10.10s %10s|",
                 bl->author[0] ? bl->author : "",
                 bl->date[0]   ? bl->date   : "");
        printw("%s", buf);
    } else {
        /* No data for this row — blank padding */
        printw("%*s", BLAME_WIDTH, "");
    }

    attroff(COLOR_PAIR(CPAIR_BLAME));
}

/*
 * draw_git_panel — render the git status panel on the right side of the screen.
 *
 * Layout (mirrors the file explorer panel pattern):
 *   Row TAB_BAR_HEIGHT       : header (" Git Status")
 *   Row TAB_BAR_HEIGHT+1 ... : status entries (one per row)
 *   Each row starts with '|' at the panel's left edge as a border separator.
 *
 * Each entry shows: "XY filename" where XY is the git status code.
 * Colors: M=yellow, A=green, D=red, ?=default (untracked).
 */
static void draw_git_panel(struct Editor *ed)
{
    GitStatusList *gs = ed->git_status;
    if (!gs) return;

    int panel_x    = ed->term_cols - GIT_PANEL_WIDTH;
    int panel_rows = ed->term_rows - TAB_BAR_HEIGHT - 1;
    int entry_rows = panel_rows - 1;  /* one row for header */

    /* Scroll adjustment — keep cursor visible */
    if (ed->git_panel_cursor < ed->git_panel_scroll)
        ed->git_panel_scroll = ed->git_panel_cursor;
    if (ed->git_panel_cursor >= ed->git_panel_scroll + entry_rows)
        ed->git_panel_scroll = ed->git_panel_cursor - entry_rows + 1;
    if (ed->git_panel_scroll < 0)
        ed->git_panel_scroll = 0;

    /* ---- Header row ---- */
    move(TAB_BAR_HEIGHT, panel_x);
    addch('|');
    attron(A_BOLD);
    printw("%-*s", GIT_PANEL_WIDTH - 1, " Git Status");
    attroff(A_BOLD);

    /* ---- Entry rows ---- */
    for (int r = 0; r < entry_rows; r++) {
        int screen_row = TAB_BAR_HEIGHT + 1 + r;
        int idx        = ed->git_panel_scroll + r;

        move(screen_row, panel_x);
        addch('|');  /* left border */

        if (idx >= gs->count) {
            /* Past last entry — blank row */
            printw("%-*s", GIT_PANEL_WIDTH - 1, "");
            continue;
        }

        GitStatusEntry *e         = &gs->entries[idx];
        int             is_cursor = (idx == ed->git_panel_cursor);

        /*
         * Build the display line: " XY filename"
         * X = index status, Y = working tree status.
         * Name is truncated to fit the panel width.
         */
        int name_width = GIT_PANEL_WIDTH - 5;  /* -1 border -3 " XY" -1 space */
        if (name_width < 1) name_width = 1;

        char line_buf[GIT_PANEL_WIDTH + 1];
        snprintf(line_buf, sizeof(line_buf), " %c%c %-*.*s",
                 e->index_status, e->work_status,
                 name_width, name_width, e->path);

        /*
         * Choose color based on the most significant status.
         * Priority: cursor highlight > modified > added > deleted > untracked.
         */
        if (is_cursor && ed->git_panel_focus) {
            attron(COLOR_PAIR(CPAIR_GIT_PANEL_CURSOR) | A_BOLD);
            printw("%-*s", GIT_PANEL_WIDTH - 1, line_buf);
            attroff(COLOR_PAIR(CPAIR_GIT_PANEL_CURSOR) | A_BOLD);
        } else if (is_cursor) {
            attron(A_BOLD);
            printw("%-*s", GIT_PANEL_WIDTH - 1, line_buf);
            attroff(A_BOLD);
        } else {
            /*
             * Color by status.
             *
             * Porcelain v1 format: "XY path"
             *   X = index (staging area) status
             *   Y = working tree status
             *
             * A file is "staged" when X is not ' ' or '?' — it has
             * changes in the index ready to commit.  A file is "unstaged"
             * when Y is not ' ' — it has working tree changes.
             *
             * Colors:
             *   - Staged only (X != ' ', Y == ' '): green (CPAIR_GIT_STAGED)
             *   - Unstaged M:  yellow (CPAIR_GIT_MODIFIED)
             *   - Unstaged D:  red    (CPAIR_GIT_DELETED)
             *   - New/added A: green  (CPAIR_GIT_ADDED)
             *   - Untracked ?: default
             *   - Both staged and unstaged: yellow (unstaged takes priority
             *     since it needs attention)
             */
            int cpair = 0;
            int is_staged   = (e->index_status != ' ' && e->index_status != '?');
            int is_unstaged = (e->work_status  != ' ' && e->work_status  != '?');

            if (is_unstaged) {
                /* Unstaged changes take priority (need action) */
                if (e->work_status == 'M') cpair = CPAIR_GIT_MODIFIED;
                else if (e->work_status == 'D') cpair = CPAIR_GIT_DELETED;
                else if (e->work_status == 'A') cpair = CPAIR_GIT_ADDED;
            } else if (is_staged) {
                /* Fully staged — green to show it's ready to commit */
                cpair = CPAIR_GIT_STAGED;
            } else if (e->index_status == '?' && e->work_status == '?') {
                /* Untracked — no color */
                cpair = 0;
            }

            if (cpair) attron(COLOR_PAIR(cpair));
            printw("%-*s", GIT_PANEL_WIDTH - 1, line_buf);
            if (cpair) attroff(COLOR_PAIR(cpair));
        }
    }
}

/*
 * draw_build_panel — render the build output panel at the bottom of the screen.
 *
 * Layout:
 *   Row (status_row - BUILD_PANEL_HEIGHT):     horizontal separator + header
 *   Rows below header:                         error/warning entries
 *   Row (status_row - 1):                      last entry row
 *
 * Each entry shows: "filepath:line: error/warning: message"
 * Errors are red, warnings are yellow, the cursor entry is highlighted.
 */
static void draw_build_panel(struct Editor *ed)
{
    BuildResult *br = ed->build_result;
    if (!br) return;

    int status_row = ed->term_rows - 1;
    int panel_top  = status_row - BUILD_PANEL_HEIGHT;
    int entry_rows = BUILD_PANEL_HEIGHT - 1;  /* one row for header */

    /* Scroll adjustment — keep cursor visible */
    if (ed->build_panel_cursor < ed->build_panel_scroll)
        ed->build_panel_scroll = ed->build_panel_cursor;
    if (ed->build_panel_cursor >= ed->build_panel_scroll + entry_rows)
        ed->build_panel_scroll = ed->build_panel_cursor - entry_rows + 1;
    if (ed->build_panel_scroll < 0)
        ed->build_panel_scroll = 0;

    /* ---- Header row: separator + build status ---- */
    move(panel_top, 0);

    /*
     * Draw a horizontal line across the full width to visually separate
     * the build panel from the editor area above it.
     */
    attron(A_BOLD);
    for (int c = 0; c < ed->term_cols; c++)
        addch(ACS_HLINE);

    /* Overwrite the left portion with the header text */
    int errs = 0, warns = 0;
    for (int i = 0; i < br->error_count; i++) {
        if (br->errors[i].is_warning) warns++;
        else errs++;
    }

    move(panel_top, 1);
    if (br->exit_status == 0) {
        attron(COLOR_PAIR(CPAIR_BUILD_SUCCESS));
        printw(" Build OK ");
        attroff(COLOR_PAIR(CPAIR_BUILD_SUCCESS));
        if (warns > 0) {
            attron(COLOR_PAIR(CPAIR_BUILD_WARNING));
            printw("(%d warning%s)", warns, warns > 1 ? "s" : "");
            attroff(COLOR_PAIR(CPAIR_BUILD_WARNING));
        }
    } else {
        attron(COLOR_PAIR(CPAIR_BUILD_ERROR));
        printw(" Build FAILED ");
        attroff(COLOR_PAIR(CPAIR_BUILD_ERROR));
        if (errs > 0 || warns > 0)
            printw("(%d error%s, %d warning%s)",
                   errs, errs != 1 ? "s" : "",
                   warns, warns != 1 ? "s" : "");
    }
    attroff(A_BOLD);

    /* ---- Entry rows ---- */
    for (int r = 0; r < entry_rows; r++) {
        int screen_row = panel_top + 1 + r;
        int idx        = ed->build_panel_scroll + r;

        move(screen_row, 0);

        if (idx >= br->error_count) {
            /* Past last entry — blank row */
            clrtoeol();
            continue;
        }

        BuildError *e  = &br->errors[idx];
        int is_cursor  = (idx == ed->build_panel_cursor);

        /*
         * Format: " filepath:line: message"
         * Truncated to fit the terminal width.
         */
        char line_buf[512];
        if (e->col > 0)
            snprintf(line_buf, sizeof(line_buf), " %s:%d:%d: %s",
                     e->filepath, e->line, e->col, e->message);
        else
            snprintf(line_buf, sizeof(line_buf), " %s:%d: %s",
                     e->filepath, e->line, e->message);

        if (is_cursor && ed->build_panel_focus) {
            attron(COLOR_PAIR(CPAIR_BUILD_CURSOR) | A_BOLD);
            printw("%-*.*s", ed->term_cols, ed->term_cols, line_buf);
            attroff(COLOR_PAIR(CPAIR_BUILD_CURSOR) | A_BOLD);
        } else {
            int cpair = e->is_warning ? CPAIR_BUILD_WARNING
                                       : CPAIR_BUILD_ERROR;
            if (is_cursor) attron(A_BOLD);
            attron(COLOR_PAIR(cpair));
            printw("%-*.*s", ed->term_cols, ed->term_cols, line_buf);
            attroff(COLOR_PAIR(cpair));
            if (is_cursor) attroff(A_BOLD);
        }
    }
}

static void draw_editor_area(struct Editor *ed)
{
    Buffer *buf = editor_current_buffer(ed);

    /*
     * panel_w — width consumed by the file explorer panel.
     * When the panel is hidden or not yet created, this is 0.
     * When the panel is visible, the editor area is narrowed by FILETREE_WIDTH
     * columns so text is never drawn under the panel.
     */
    int panel_w = (ed->show_filetree && ed->filetree) ? FILETREE_WIDTH : 0;

    /*
     * Number of rows available for text content.
     * We subtract 1 for the status bar at the bottom and TAB_BAR_HEIGHT
     * for the tab bar at the top.
     */
    int text_rows = ed->term_rows - 1 - TAB_BAR_HEIGHT;
    if (ed->show_build_panel && ed->build_result)
        text_rows -= BUILD_PANEL_HEIGHT;
    if (text_rows < 1) text_rows = 1;

    /*
     * Pre-compute selection bounds once, outside the row loop.
     *
     * The selection is defined by an anchor point (where Shift was first
     * pressed) and the current cursor.  We normalize so that sel_sr/sel_sc
     * is always the earlier position and sel_er/sel_ec the later one.
     *
     * If no selection is active, sel_active is 0 and we skip all of this.
     */
    int sel_active = ed->sel_active;
    int sel_sr = 0, sel_sc = 0, sel_er = 0, sel_ec = 0;

    if (sel_active) {
        int ar = ed->sel_anchor_row, ac = ed->sel_anchor_col;
        int cr = ed->cursor_row,     cc = ed->cursor_col;

        if (ar < cr || (ar == cr && ac <= cc)) {
            sel_sr = ar; sel_sc = ac;
            sel_er = cr; sel_ec = cc;
        } else {
            sel_sr = cr; sel_sc = cc;
            sel_er = ar; sel_ec = ac;
        }
    }

    /*
     * text_cols — usable column width for text.
     * We subtract the gutter width (line numbers) and the file-tree panel
     * width (when visible) from the total terminal width.
     */
    /*
     * git_panel_w — width consumed by the git status panel on the right side.
     */
    int git_panel_w = (ed->show_git_panel && ed->git_status) ? GIT_PANEL_WIDTH : 0;
    int blame_w     = ed->show_git_blame ? BLAME_WIDTH : 0;

    int text_cols = ed->term_cols - GUTTER_WIDTH - panel_w - git_panel_w - blame_w;

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
        for (int r = 0; r < ed->view_row && r < buf->num_lines; r++) {
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
        int buf_row = ed->view_row;
        int segment = 0;           /* which wrapped segment of buf_row */

        /* Inline diff chunk index for word-wrap path (same pattern as no-wrap) */
        int chunk_idx_wr = 0;
        if (ed->show_inline_diff) {
            while (chunk_idx_wr < ed->inline_diff.count
                   && ed->inline_diff.chunks[chunk_idx_wr].before_line
                      < ed->view_row)
                chunk_idx_wr++;
        }

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
            if (ed->region_active && segment == 0
                    && buf_row == ed->region_start_row) {
                draw_region_hborder(screen_row, panel_w, ed, 1);
                screen_row++;
                if (screen_row >= text_rows) break;
            }

            /*
             * ---- Inline diff phantom lines (word-wrap) ----
             * Same logic as the no-wrap path: draw old lines before the
             * first segment (segment == 0) of the buffer line they reference.
             */
            if (ed->show_inline_diff && segment == 0) {
                while (chunk_idx_wr < ed->inline_diff.count
                       && ed->inline_diff.chunks[chunk_idx_wr].before_line
                          == buf_row) {
                    GitDiffChunk *c = &ed->inline_diff.chunks[chunk_idx_wr];
                    for (int j = 0; j < c->old_count
                                    && screen_row < text_rows; j++) {
                        move(TAB_BAR_HEIGHT + screen_row, panel_w);
                        draw_phantom_line(c->old_lines[j], text_cols,
                                          ed->show_whitespace, blame_w);
                        screen_row++;
                    }
                    chunk_idx_wr++;
                    if (screen_row >= text_rows) break;
                }
                if (screen_row >= text_rows) break;
            }

            /*
             * Move to the start of this screen row, offset right by panel_w
             * so we start drawing after the file-tree panel (if visible).
             */
            move(TAB_BAR_HEIGHT + screen_row, panel_w);

            if (buf_row >= buf->num_lines) {
                /* Past end of file */
                syn_tokens = NULL;
                attron(COLOR_PAIR(CPAIR_GUTTER));
                printw("  ~  ");
                attroff(COLOR_PAIR(CPAIR_GUTTER));
                if (blame_w > 0)
                    printw("%*s", blame_w, "");
                clrtoeol();
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

            int is_cursor_row = (buf_row == ed->cursor_row);
            int row_attr      = A_NORMAL;  /* no full-line highlight */

            /*
             * in_region — true if this buffer row falls within the region
             * marked by the user with Ctrl+U.  Used to draw a red box border
             * in the gutter and at the right edge of each region row.
             */
            int in_region = ed->region_active
                            && buf_row >= ed->region_start_row
                            && buf_row <= ed->region_end_row;

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
                     * First segment: indicator + line number.
                     * Column 0: LSP diagnostic or git marker.
                     * Columns 1-4: right-aligned line number.
                     */
                    attroff(COLOR_PAIR(CPAIR_GUTTER));
                    if (!draw_lsp_line_indicator(buf, buf_row))
                        draw_git_marker(&buf->git_state, buf_row);
                    attron(COLOR_PAIR(CPAIR_GUTTER));
                    if (is_cursor_row) attron(A_BOLD);
                    printw("%3d ", buf_row + 1);
                    if (is_cursor_row) attroff(A_BOLD);
                    attroff(COLOR_PAIR(CPAIR_GUTTER));
                } else {
                    /*
                     * Continuation segment: leave gutter blank (no line number)
                     * so the wrapped text looks like a continuation of the line.
                     */
                    printw("     ");
                    attroff(COLOR_PAIR(CPAIR_GUTTER));
                }
            }

            /* ---- Blame annotation column (word-wrap path) ---- */
            if (ed->show_git_blame) {
                if (segment == 0)
                    draw_blame_annotation(&ed->git_blame, buf_row);
                else
                    printw("%*s", BLAME_WIDTH, "");  /* blank for continuation */
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
                                || (bm_col >= 0 && buf_row == ed->cursor_row)
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
                clrtoeol();
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
                    clrtoeol();
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
                    clrtoeol();
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
                mvaddch(TAB_BAR_HEIGHT + screen_row, ed->term_cols - 1, ACS_VLINE);
                attroff(COLOR_PAIR(CPAIR_REGION));
            }

            /* ---- Advance (buf_row, segment) ---- */
            int was_region_end = (ed->region_active
                                  && buf_row == ed->region_end_row);
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
                draw_region_hborder(screen_row, panel_w, ed, 0);
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

        /*
         * chunk_idx — index into ed->inline_diff.chunks[] for the next
         * chunk whose phantom lines haven't been rendered yet.  Chunks
         * are sorted by before_line (ascending, matching diff order).
         * We skip chunks that are above the viewport.
         */
        int chunk_idx = 0;
        if (ed->show_inline_diff) {
            while (chunk_idx < ed->inline_diff.count
                   && ed->inline_diff.chunks[chunk_idx].before_line < ed->view_row)
                chunk_idx++;
        }

        int buf_row = ed->view_row;
        int screen_row = 0;
        while (screen_row < text_rows) {
            /*
             * ---- Region top border ----
             * Before the region start row, draw a horizontal line
             * ┌──────────┐ across the full editor width.
             */
            if (ed->region_active && buf_row == ed->region_start_row) {
                draw_region_hborder(screen_row, panel_w, ed, 1);
                screen_row++;
                if (screen_row >= text_rows) break;
            }

            /*
             * ---- Inline diff phantom lines ----
             * Before rendering this buffer line, draw any old (deleted)
             * lines from the diff that belong at this position.
             *
             * Phantom lines are "attached" to their before_line: they
             * appear on screen immediately above the buffer line they
             * reference.  Multiple chunks can start at the same line
             * (e.g. if two hunks coincide), so we use a while loop.
             */
            if (ed->show_inline_diff) {
                while (chunk_idx < ed->inline_diff.count
                       && ed->inline_diff.chunks[chunk_idx].before_line == buf_row) {
                    GitDiffChunk *c = &ed->inline_diff.chunks[chunk_idx];
                    for (int j = 0; j < c->old_count && screen_row < text_rows; j++) {
                        move(TAB_BAR_HEIGHT + screen_row, panel_w);
                        draw_phantom_line(c->old_lines[j], text_cols,
                                          ed->show_whitespace, blame_w);
                        screen_row++;
                    }
                    chunk_idx++;
                    if (screen_row >= text_rows) break;
                }
                if (screen_row >= text_rows) break;
            }

            /*
             * Position at the start of this screen row, offset right by panel_w
             * so we draw in the editor area (right of the file-tree panel).
             */
            move(TAB_BAR_HEIGHT + screen_row, panel_w);

            if (buf_row >= buf->num_lines) {
                /*
                 * Past end of file — draw a tilde in the gutter (like vim does)
                 * to indicate "no line here".
                 */
                attron(COLOR_PAIR(CPAIR_GUTTER));
                printw("  ~  ");
                attroff(COLOR_PAIR(CPAIR_GUTTER));
                if (blame_w > 0)
                    printw("%*s", blame_w, "");
                clrtoeol();
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
            int is_cursor_row = (buf_row == ed->cursor_row);

            /*
             * in_region — true if this row falls inside the user's marked region.
             */
            int in_region = ed->region_active
                            && buf_row >= ed->region_start_row
                            && buf_row <= ed->region_end_row;

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
                /*
                 * Column 0: indicator marker (LSP diagnostic > git change).
                 * Columns 1-4: line number + space.
                 *
                 * LSP diagnostics take priority over git markers in column 0
                 * because errors/warnings are more actionable than change status.
                 */
                if (!draw_lsp_line_indicator(buf, buf_row))
                    draw_git_marker(&buf->git_state, buf_row);
                attron(COLOR_PAIR(CPAIR_GUTTER));
                if (is_cursor_row) attron(A_BOLD);
                printw("%3d ", buf_row + 1);
                if (is_cursor_row) attroff(A_BOLD);
                attroff(COLOR_PAIR(CPAIR_GUTTER));
            }

            /* ---- Blame annotation column (no-wrap path) ---- */
            if (ed->show_git_blame)
                draw_blame_annotation(&ed->git_blame, buf_row);

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
            int         view_col  = ed->view_col;

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
                                 || (bm_col >= 0 && buf_row == ed->cursor_row)
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
                clrtoeol();
                attrset(A_NORMAL);

            } else if (!has_selection_on_row) {
                /* No search, no selection — render the whole line with row_attr */
                attrset(row_attr);
                if (draw_len > 0)
                    draw_text_segment(draw_start, draw_len, ed->show_whitespace);
                clrtoeol();
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
                clrtoeol();
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
                mvaddch(TAB_BAR_HEIGHT + screen_row, ed->term_cols - 1, ACS_VLINE);
                attroff(COLOR_PAIR(CPAIR_REGION));
            }

            /* ---- Advance ---- */
            int was_region_end = (ed->region_active
                                  && buf_row == ed->region_end_row);
            buf_row++;
            screen_row++;

            /*
             * ---- Region bottom border ----
             * After the region end row, draw a horizontal line
             * └──────────┘ across the full editor width.
             */
            if (was_region_end && screen_row < text_rows) {
                draw_region_hborder(screen_row, panel_w, ed, 0);
                screen_row++;
            }
        }
    }
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
             ed->cursor_row + 1,
             ed->cursor_col + 1);

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
    /*
     * Status bar center content — priority order:
     *   1. LSP diagnostic on the cursor line (always wins — most actionable)
     *   2. Explicit status message (from editor_set_status)
     *   3. Default key hint
     */
    const char *diag_msg = NULL;
    int diag_sev = 0;
    if (buf->lsp_diagnostics.count > 0) {
        for (int i = 0; i < buf->lsp_diagnostics.count; i++) {
            if (buf->lsp_diagnostics.items[i].line == ed->cursor_row) {
                diag_msg = buf->lsp_diagnostics.items[i].message;
                diag_sev = buf->lsp_diagnostics.items[i].severity;
                break;
            }
        }
    }

    if (diag_msg && diag_msg[0]) {
        /* LSP diagnostic on cursor line — show in error/warning color */
        int diag_cpair = (diag_sev == LSP_SEV_ERROR)
                         ? CPAIR_LSP_ERROR : CPAIR_LSP_WARNING;
        int msg_col = (ed->term_cols - (int)strlen(diag_msg)) / 2;
        if (msg_col < (int)strlen(left) + 1) msg_col = (int)strlen(left) + 1;
        attron(COLOR_PAIR(diag_cpair) | A_BOLD);
        mvprintw(status_row, msg_col, "%.80s", diag_msg);
        attroff(COLOR_PAIR(diag_cpair) | A_BOLD);
    } else if (ed->status_msg[0] != '\0') {
        /* Explicit status message */
        int msg_col = (ed->term_cols - (int)strlen(ed->status_msg)) / 2;
        if (msg_col < 0) msg_col = 0;
        attron(COLOR_PAIR(color) | A_BOLD);
        mvprintw(status_row, msg_col, "%.80s", ed->status_msg);
        attroff(COLOR_PAIR(color) | A_BOLD);
    } else {
        /* Default hint */
        const char *hint = "Ctrl+S save  Ctrl+Q quit";
        int hint_col = (ed->term_cols - (int)strlen(hint)) / 2;
        if (hint_col > (int)strlen(left) && hint_col > 0) {
            attron(COLOR_PAIR(color));
            mvprintw(status_row, hint_col, "%s", hint);
            attroff(COLOR_PAIR(color));
        }
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

    if (ed->show_git_panel && ed->git_status)
        draw_git_panel(ed);

    if (ed->show_build_panel && ed->build_result)
        draw_build_panel(ed);

    draw_status_bar(ed);

    /*
     * Position the terminal cursor.
     *
     * There are two cases:
     *   A) Filetree has focus: put the cursor on the highlighted tree entry.
     *   B) Editor has focus:   put the cursor at the editor cursor position.
     *
     * panel_w is the horizontal offset of the editor area from the left edge.
     */
    int panel_w = (ed->show_filetree && ed->filetree) ? FILETREE_WIDTH : 0;
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
         * text_cols_wr must account for the panel width so wrapping matches
         * what draw_editor_area actually rendered.
         */
        int blame_w_wr   = ed->show_git_blame ? BLAME_WIDTH : 0;
        int git_pw_wr    = (ed->show_git_panel && ed->git_status) ? GIT_PANEL_WIDTH : 0;
        int text_cols_wr = ed->term_cols - GUTTER_WIDTH - panel_w - blame_w_wr - git_pw_wr;
        if (text_cols_wr <= 0) text_cols_wr = 1;

        /* Count screen rows consumed by lines before cursor_row */
        int sr = 0;
        Buffer *buf_wr = editor_current_buffer(ed);
        if (buf_wr) {
            for (int r = ed->view_row; r < ed->cursor_row; r++) {
                int len = buffer_line_len(buf_wr, r);
                sr += line_screen_rows_d(len, text_cols_wr);
            }
        }
        /* Add the sub-row within the cursor line */
        sr += ed->cursor_col / text_cols_wr;

        screen_row = TAB_BAR_HEIGHT + sr;
        screen_col = panel_w + GUTTER_WIDTH + blame_w_wr + (ed->cursor_col % text_cols_wr);

        /*
         * Adjust for region border rows that consume screen space.
         * The top border is drawn before region_start_row, so if the
         * cursor is at or past that row, shift down by 1.  Similarly
         * the bottom border is drawn after region_end_row.
         */
        if (ed->region_active) {
            if (ed->cursor_row >= ed->region_start_row)
                screen_row++;          /* top border pushed us down */
            if (ed->cursor_row > ed->region_end_row)
                screen_row++;          /* bottom border pushed us down */
        }

        /* Inline diff phantom lines (same adjustment as normal mode) */
        if (ed->show_inline_diff) {
            screen_row += git_phantom_lines_in_range(
                &ed->inline_diff,
                ed->view_row, ed->cursor_row + 1);
        }

    } else {
        /*
         * Case B (normal mode): straightforward linear mapping.
         * Add panel_w so the cursor lands in the editor area, not in the panel.
         */
        {
        int blame_w_nm = ed->show_git_blame ? BLAME_WIDTH : 0;
        screen_col = panel_w + GUTTER_WIDTH + blame_w_nm + (ed->cursor_col - ed->view_col);
        screen_row = TAB_BAR_HEIGHT + (ed->cursor_row - ed->view_row);

        /* Adjust for region border rows (same logic as word-wrap case) */
        if (ed->region_active) {
            if (ed->cursor_row >= ed->region_start_row)
                screen_row++;
            if (ed->cursor_row > ed->region_end_row)
                screen_row++;
        }

        /*
         * Adjust for inline diff phantom lines.
         *
         * Phantom lines appear above their associated buffer line, pushing
         * all subsequent screen rows down.  We count all phantom lines
         * between view_row and cursor_row (inclusive) to compute the offset.
         *
         * We use cursor_row + 1 as the upper bound so that phantom lines
         * at before_line == cursor_row (which appear above the cursor's
         * line on screen) are included in the offset.
         */
        if (ed->show_inline_diff) {
            screen_row += git_phantom_lines_in_range(
                &ed->inline_diff,
                ed->view_row, ed->cursor_row + 1);
        }
        }  /* end of blame_w_nm block */
    }

    /* Clamp to prevent the cursor from escaping the text area */
    int text_area_top    = TAB_BAR_HEIGHT;
    int text_area_bottom = ed->term_rows - 2;   /* one above the status bar */
    int blame_w_clamp    = ed->show_git_blame ? BLAME_WIDTH : 0;
    int min_col          = (ed->filetree_focus && ed->show_filetree && ed->filetree)
                           ? 0
                           : panel_w + GUTTER_WIDTH + blame_w_clamp;

    if (screen_row < text_area_top)    screen_row = text_area_top;
    if (screen_row > text_area_bottom) screen_row = text_area_bottom;
    if (screen_col < min_col)          screen_col = min_col;
    if (screen_col >= ed->term_cols)   screen_col = ed->term_cols - 1;

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

/* ============================================================================
 * display_finder_popup — fuzzy file finder overlay
 * ============================================================================ */

char *display_finder_popup(struct Editor *ed,
                           FinderFile *files, int num_files)
{
    (void)ed;  /* used only for term dimensions via LINES/COLS */

    /*
     * Popup dimensions: centered on screen, reasonable defaults.
     * Width:  min(COLS - 4, 80)    — most of the screen width
     * Height: min(LINES - 4, 20)   — enough for a good result list
     */
    int popup_w = COLS - 4;
    if (popup_w > 80) popup_w = 80;
    if (popup_w < 20) popup_w = 20;

    int popup_h = LINES - 4;
    if (popup_h > 20) popup_h = 20;
    if (popup_h < 5)  popup_h = 5;

    int popup_x = (COLS - popup_w) / 2;
    int popup_y = (LINES - popup_h) / 2;

    /* Number of result rows (popup height minus border rows and input row) */
    int result_rows = popup_h - 3;  /* top border + input row + bottom border */
    if (result_rows < 1) result_rows = 1;

    /* Query and results state */
    char query[FINDER_QUERY_MAX];
    query[0] = '\0';
    int query_len = 0;

    FinderResult *results = malloc(FINDER_MAX_RESULTS * sizeof(FinderResult));
    if (!results) return NULL;

    int num_results = 0;
    int cursor = 0;
    int scroll = 0;

    /* Initial filter (empty query → show all files) */
    num_results = finder_filter(files, num_files, query,
                                results, FINDER_MAX_RESULTS);

    for (;;) {
        /*
         * ---- Draw the popup ----
         *
         * We draw directly onto stdscr as an overlay.  When the popup
         * closes, the caller calls display_render() to redraw everything.
         */

        /* Inner width available for text (popup_w minus 2 border columns) */
        int inner_w = popup_w - 2;
        if (inner_w < 1) inner_w = 1;

        /* ---- Top border ---- */
        attron(COLOR_PAIR(CPAIR_FINDER_BORDER));
        mvaddch(popup_y, popup_x, ACS_ULCORNER);
        mvhline(popup_y, popup_x + 1, ACS_HLINE, popup_w - 2);
        mvaddch(popup_y, popup_x + popup_w - 1, ACS_URCORNER);

        /* Title in the border */
        mvprintw(popup_y, popup_x + 2, " Find File ");
        attroff(COLOR_PAIR(CPAIR_FINDER_BORDER));

        /* ---- Input row ---- */
        int input_row = popup_y + 1;
        attron(COLOR_PAIR(CPAIR_FINDER_BORDER));
        mvaddch(input_row, popup_x, ACS_VLINE);
        mvaddch(input_row, popup_x + popup_w - 1, ACS_VLINE);
        attroff(COLOR_PAIR(CPAIR_FINDER_BORDER));

        /* Draw the query input: "> query_text" */
        move(input_row, popup_x + 1);
        attron(A_BOLD);
        printw("> ");
        attroff(A_BOLD);
        printw("%-*.*s", inner_w - 2, inner_w - 2, query);

        /* ---- Result rows ---- */
        /* Scroll to keep cursor visible */
        if (cursor < scroll) scroll = cursor;
        if (cursor >= scroll + result_rows)
            scroll = cursor - result_rows + 1;
        if (scroll < 0) scroll = 0;

        for (int r = 0; r < result_rows; r++) {
            int screen_row = popup_y + 2 + r;
            int idx = scroll + r;

            /* Left border */
            attron(COLOR_PAIR(CPAIR_FINDER_BORDER));
            mvaddch(screen_row, popup_x, ACS_VLINE);
            mvaddch(screen_row, popup_x + popup_w - 1, ACS_VLINE);
            attroff(COLOR_PAIR(CPAIR_FINDER_BORDER));

            move(screen_row, popup_x + 1);

            if (idx >= num_results) {
                /* Blank row */
                printw("%*s", inner_w, "");
                continue;
            }

            /* Get the display text for this result */
            const char *disp = files[results[idx].index].display;
            int disp_len = (int)strlen(disp);

            /* Truncate to fit */
            int show_len = disp_len;
            if (show_len > inner_w) show_len = inner_w;

            int is_cursor_row = (idx == cursor);

            if (is_cursor_row) {
                attron(COLOR_PAIR(CPAIR_FINDER_CURSOR) | A_BOLD);
                printw(" %-*.*s", inner_w - 1, inner_w - 1, disp);
                attroff(COLOR_PAIR(CPAIR_FINDER_CURSOR) | A_BOLD);
            } else {
                printw(" %-*.*s", inner_w - 1, inner_w - 1, disp);
            }
        }

        /* ---- Bottom border ---- */
        int bottom_row = popup_y + popup_h - 1;
        attron(COLOR_PAIR(CPAIR_FINDER_BORDER));
        mvaddch(bottom_row, popup_x, ACS_LLCORNER);
        mvhline(bottom_row, popup_x + 1, ACS_HLINE, popup_w - 2);
        mvaddch(bottom_row, popup_x + popup_w - 1, ACS_LRCORNER);

        /* Result count in the bottom border */
        char count_str[32];
        snprintf(count_str, sizeof(count_str), " %d/%d ", num_results, num_files);
        mvprintw(bottom_row, popup_x + popup_w - 2 - (int)strlen(count_str),
                 "%s", count_str);
        attroff(COLOR_PAIR(CPAIR_FINDER_BORDER));

        /* Position the cursor at the end of the query text */
        move(input_row, popup_x + 3 + query_len);
        refresh();

        /* ---- Handle input ---- */
        int key = getch();

        if (key == 27) {
            /* Escape — cancel */
            free(results);
            return NULL;

        } else if (key == '\n' || key == '\r' || key == KEY_ENTER) {
            /* Enter — select the current result */
            if (num_results > 0 && cursor >= 0 && cursor < num_results) {
                char *selected = strdup(files[results[cursor].index].path);
                free(results);
                return selected;
            }
            /* No results — ignore Enter */

        } else if (key == KEY_UP) {
            if (cursor > 0) cursor--;

        } else if (key == KEY_DOWN) {
            if (cursor < num_results - 1) cursor++;

        } else if (key == KEY_BACKSPACE || key == 127 || key == '\b') {
            /* Backspace — remove last character from query */
            if (query_len > 0) {
                query[--query_len] = '\0';
                cursor = 0;
                scroll = 0;
                num_results = finder_filter(files, num_files, query,
                                            results, FINDER_MAX_RESULTS);
            }

        } else if (key == KEY_RESIZE) {
            /* Terminal resized — recalculate popup dimensions */
            popup_w = COLS - 4;
            if (popup_w > 80) popup_w = 80;
            if (popup_w < 20) popup_w = 20;
            popup_h = LINES - 4;
            if (popup_h > 20) popup_h = 20;
            if (popup_h < 5)  popup_h = 5;
            popup_x = (COLS - popup_w) / 2;
            popup_y = (LINES - popup_h) / 2;
            result_rows = popup_h - 3;
            if (result_rows < 1) result_rows = 1;
            erase();  /* clear the whole screen for redraw */

        } else if (key >= 0x20 && key <= 0x7e
                   && query_len < FINDER_QUERY_MAX - 1) {
            /* Printable character — append to query and re-filter */
            query[query_len++] = (char)key;
            query[query_len]   = '\0';
            cursor = 0;
            scroll = 0;
            num_results = finder_filter(files, num_files, query,
                                        results, FINDER_MAX_RESULTS);
        }
    }
}
