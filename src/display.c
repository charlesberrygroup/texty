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

static void draw_editor_area(struct Editor *ed)
{
    Buffer *buf = editor_current_buffer(ed);

    /*
     * Number of rows available for text content.
     * We subtract 1 for the status bar at the bottom and TAB_BAR_HEIGHT
     * for the tab bar at the top.
     */
    int text_rows = ed->term_rows - 1 - TAB_BAR_HEIGHT;

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
     * text_cols — usable column width for text (terminal width minus gutter).
     * Needed by both the normal and word-wrap rendering paths.
     */
    int text_cols = ed->term_cols - GUTTER_WIDTH;

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

        for (int screen_row = 0; screen_row < text_rows; screen_row++) {
            move(TAB_BAR_HEIGHT + screen_row, 0);

            if (buf_row >= buf->num_lines) {
                /* Past end of file */
                syn_tokens = NULL;
                attron(COLOR_PAIR(CPAIR_GUTTER));
                printw("  ~  ");
                attroff(COLOR_PAIR(CPAIR_GUTTER));
                clrtoeol();
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
            int row_attr      = is_cursor_row ? A_REVERSE : A_NORMAL;

            /* ---- Gutter ---- */
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

            /* ---- Advance (buf_row, segment) ---- */
            segment++;
            if (start_col + text_cols >= line_len) {
                /* We have rendered the last (or only) segment of this line */
                buf_row++;
                segment = 0;
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

        for (int screen_row = 0; screen_row < text_rows; screen_row++) {
            int buf_row = ed->view_row + screen_row;

            /* Position at the start of this screen row (offset past the tab bar) */
            move(TAB_BAR_HEIGHT + screen_row, 0);

            if (buf_row >= buf->num_lines) {
                /*
                 * Past end of file — draw a tilde in the gutter (like vim does)
                 * to indicate "no line here".
                 */
                attron(COLOR_PAIR(CPAIR_GUTTER));
                printw("  ~  ");
                attroff(COLOR_PAIR(CPAIR_GUTTER));
                clrtoeol();
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

            /* ---- Gutter: line number ---------------------------------------- */
            int is_cursor_row = (buf_row == ed->cursor_row);

            attron(COLOR_PAIR(CPAIR_GUTTER));
            if (is_cursor_row) attron(A_BOLD);
            printw("%4d ", buf_row + 1);
            if (is_cursor_row) attroff(A_BOLD);
            attroff(COLOR_PAIR(CPAIR_GUTTER));

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
             * Cursor row uses A_REVERSE (reverse video) to highlight the line.
             * Other rows use A_NORMAL (terminal defaults).
             * Selected segments always use CPAIR_SELECTION regardless of row.
             */
            int row_attr = is_cursor_row ? A_REVERSE : A_NORMAL;

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
    draw_editor_area(ed);
    draw_status_bar(ed);

    /*
     * Position the terminal cursor at the editor cursor position.
     *
     * In normal mode:
     *   screen_col = GUTTER_WIDTH + (cursor_col - view_col)
     *   screen_row = TAB_BAR_HEIGHT + (cursor_row - view_row)
     *
     * In word-wrap mode, each buffer line can span multiple screen rows,
     * so we must count the actual screen rows consumed by all lines between
     * view_row and cursor_row, then add the cursor's sub-row within its line.
     */
    int screen_col, screen_row;

    if (ed->word_wrap) {
        int text_cols_wr = ed->term_cols - GUTTER_WIDTH;
        if (text_cols_wr <= 0) text_cols_wr = 1;

        /* Count screen rows from view_row up to (but not including) cursor_row */
        int sr = 0;
        Buffer *buf_wr = editor_current_buffer(ed);
        if (buf_wr) {
            for (int r = ed->view_row; r < ed->cursor_row; r++) {
                int len = buffer_line_len(buf_wr, r);
                sr += line_screen_rows_d(len, text_cols_wr);
            }
        }
        /* Add the sub-row within cursor_row */
        sr += ed->cursor_col / text_cols_wr;

        screen_row = TAB_BAR_HEIGHT + sr;
        screen_col = GUTTER_WIDTH  + (ed->cursor_col % text_cols_wr);
    } else {
        screen_col = GUTTER_WIDTH + (ed->cursor_col - ed->view_col);
        screen_row = TAB_BAR_HEIGHT + (ed->cursor_row - ed->view_row);
    }

    /* Clamp to prevent moving outside the text area */
    int text_area_top    = TAB_BAR_HEIGHT;
    int text_area_bottom = ed->term_rows - 2;   /* above the status bar */

    if (screen_row < text_area_top)    screen_row = text_area_top;
    if (screen_row > text_area_bottom) screen_row = text_area_bottom;
    if (screen_col < GUTTER_WIDTH)     screen_col = GUTTER_WIDTH;
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
