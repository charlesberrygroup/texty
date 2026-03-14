/*
 * display.h — Terminal Rendering (ncurses)
 * =============================================================================
 * This module owns everything related to drawing on the terminal screen.
 * It uses the ncurses library to position text, apply colors, and read the
 * terminal size.
 *
 * Screen layout
 * -------------
 *
 *   col:  0         GUTTER_WIDTH           term_cols-1
 *         |              |                      |
 *   row 0 |  1           |  line 1 text ...     |
 *   row 1 |  2           |  line 2 text ...     |
 *       . |  .           |  ...                 |
 *       . |  .           |  ...                 |
 *  last-1 | [status bar: filename  ln:col  mode]|
 *
 * The gutter shows 4-digit line numbers followed by a space (e.g. "  42 ").
 * The status bar is a highlighted row at the very bottom of the terminal.
 *
 * ncurses coordinate convention: move(row, col) — row 0 is the TOP of the
 * screen, col 0 is the LEFT edge.
 * =============================================================================
 */

#ifndef DISPLAY_H
#define DISPLAY_H

/* Forward declaration — avoids a circular include with editor.h */
struct Editor;

/* ---- Constants ------------------------------------------------------------ */

/**
 * Width of the line-number gutter in columns.
 *
 * 4 digits + 1 space = "9999 " fits lines up to 9,999.
 * If you want 5-digit support, bump this to 6.
 */
#define GUTTER_WIDTH    5

/**
 * Height of the tab bar in rows.  The tab bar occupies the very top row of
 * the terminal and shows all open buffers, highlighting the current one.
 */
#define TAB_BAR_HEIGHT  1

/* ---- Color pair IDs -------------------------------------------------------
 *
 * ncurses uses numbered "color pairs" — a foreground/background combination.
 * We define symbolic names here so the rest of the code never uses magic
 * numbers when calling COLOR_PAIR().
 *
 * NOTE: CPAIR_DEFAULT and CPAIR_CURLINE are intentionally NOT registered with
 * init_pair() in display.c.  On macOS, the system ncurses has a bug where
 * passing -1 (terminal default) to init_pair() results in COLOR_BLACK instead
 * of the actual terminal default, which causes black-on-black invisible text.
 * Instead:
 *   - Normal text uses attrset(A_NORMAL) — clears all attributes.
 *   - Cursor-line highlight uses A_REVERSE — swaps fg/bg, works everywhere.
 */
#define CPAIR_DEFAULT      1   /* Reserved — not currently used          */
#define CPAIR_GUTTER       2   /* Line number text                       */
#define CPAIR_STATUS       3   /* Status bar (clean file)                */
#define CPAIR_STATUS_DIRTY 4   /* Status bar when file has unsaved edits */
#define CPAIR_CURLINE      5   /* Reserved — not currently used          */
#define CPAIR_SELECTION    6   /* Selected text highlight                */
#define CPAIR_TAB_ACTIVE   7   /* Active buffer tab in the tab bar       */
#define CPAIR_TAB_INACTIVE 8   /* Inactive buffer tab in the tab bar     */
#define CPAIR_SEARCH_MATCH 9   /* A search match (not the current one)   */
#define CPAIR_SEARCH_CUR  10   /* The currently highlighted search match */
#define CPAIR_BRACKET     11   /* Matching bracket pair highlight        */

/* ---- Syntax-highlighting color pairs (used by display.c + syntax.c) ------- */
#define CPAIR_SYN_KEYWORD 12   /* Language keywords: if, for, return...  */
#define CPAIR_SYN_TYPE    13   /* Built-in types: int, void, bool...     */
#define CPAIR_SYN_STRING  14   /* String / character literals            */
#define CPAIR_SYN_COMMENT 15   /* Comments (line or block)               */
#define CPAIR_SYN_PREPROC 16   /* Preprocessor directives: #include...   */
#define CPAIR_SYN_NUMBER  17   /* Numeric literals: 42, 0xFF, 3.14       */

/* ---- Functions ------------------------------------------------------------ */

/**
 * display_init — set up ncurses.
 *
 * Call once at startup, before any rendering.  Configures raw mode, disables
 * echo, enables special-key recognition, and sets up color pairs.
 */
void display_init(void);

/**
 * display_cleanup — restore the terminal to its original state.
 *
 * Call once before the program exits.
 */
void display_cleanup(void);

/**
 * display_update_size — read the current terminal dimensions into the editor.
 *
 * Call at startup and whenever SIGWINCH fires (terminal was resized).
 */
void display_update_size(struct Editor *ed);

/**
 * display_render — redraw the entire screen.
 *
 * Call once per iteration of the main event loop, after processing input.
 * Draws:
 *   1. The tab bar at the top (row 0)
 *   2. The text content of the current buffer (rows 1..term_rows-2)
 *   3. The status bar at the bottom (row term_rows-1)
 *   4. Positions the terminal cursor at the editor cursor position
 */
void display_render(struct Editor *ed);

/**
 * display_prompt — show an inline prompt in the status bar and read input.
 *
 * Draws `prompt` in the status bar, then lets the user type a response.
 * Supports Backspace to delete and Escape to cancel.
 *
 * Returns a heap-allocated string (must be free()'d by the caller) when
 * the user presses Enter, or NULL if they pressed Escape.
 */
char *display_prompt(struct Editor *ed, const char *prompt);

#endif /* DISPLAY_H */
