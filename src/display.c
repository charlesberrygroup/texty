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
 * Internal: draw the editor area (buffer content + gutter)
 * ============================================================================ */

static void draw_editor_area(struct Editor *ed)
{
    Buffer *buf = editor_current_buffer(ed);

    /* Number of rows available for text (everything above the status bar) */
    int text_rows = ed->term_rows - 1;

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

    for (int screen_row = 0; screen_row < text_rows; screen_row++) {
        int buf_row = ed->view_row + screen_row;

        /* Position at the start of this screen row */
        move(screen_row, 0);

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
        int         text_cols = ed->term_cols - GUTTER_WIDTH;
        int         view_col  = ed->view_col;

        /* Clamp view_col so we don't go past end of line */
        int start_col = (view_col <= line_len) ? view_col : line_len;
        int draw_len  = line_len - start_col;
        if (draw_len > text_cols) draw_len = text_cols;

        /*
         * draw_start points to the first character that will be rendered.
         * All column positions below are relative to the start of the line
         * (buffer coordinates), so we subtract view_col to convert to screen
         * coordinates within the text area.
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

        if (!has_selection_on_row) {
            /* No selection on this row — render the whole line with row_attr */
            attrset(row_attr);
            if (draw_len > 0)
                addnstr(draw_start, draw_len);
        } else {
            /*
             * Split the line into three segments and render each with the
             * correct attribute:
             *
             *   [0 .. vis_sel_start)          row_attr   (before selection)
             *   [vis_sel_start .. vis_sel_end) CPAIR_SELECTION
             *   [vis_sel_end .. draw_len)      row_attr   (after selection)
             */

            /* Segment 1: before the selection */
            if (vis_sel_start > 0) {
                attrset(row_attr);
                addnstr(draw_start, vis_sel_start);
            }

            /* Segment 2: inside the selection */
            attrset(A_NORMAL);
            attron(COLOR_PAIR(CPAIR_SELECTION));
            if (vis_sel_end > vis_sel_start) {
                int seg_len = vis_sel_end - vis_sel_start;
                addnstr(draw_start + vis_sel_start, seg_len);
            }
            attroff(COLOR_PAIR(CPAIR_SELECTION));

            /* Segment 3: after the selection */
            if (vis_sel_end < draw_len) {
                attrset(row_attr);
                addnstr(draw_start + vis_sel_end, draw_len - vis_sel_end);
            }
        }

        /* Fill the rest of the line with the row's base attribute */
        attrset(row_attr);
        clrtoeol();
        attrset(A_NORMAL);
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

    draw_editor_area(ed);
    draw_status_bar(ed);

    /*
     * Position the terminal cursor at the editor cursor position.
     *
     * Screen column = GUTTER_WIDTH + (cursor_col - view_col)
     * Screen row    = cursor_row - view_row
     */
    int screen_col = GUTTER_WIDTH + (ed->cursor_col - ed->view_col);
    int screen_row = ed->cursor_row - ed->view_row;

    /* Clamp to prevent moving outside the visible area */
    if (screen_row < 0)                  screen_row = 0;
    if (screen_row >= ed->term_rows - 1) screen_row = ed->term_rows - 2;
    if (screen_col < GUTTER_WIDTH)       screen_col = GUTTER_WIDTH;
    if (screen_col >= ed->term_cols)     screen_col = ed->term_cols - 1;

    move(screen_row, screen_col);

    /*
     * refresh() sends the accumulated changes to the actual terminal.
     * Nothing is visible on screen until refresh() is called.
     */
    refresh();
}
