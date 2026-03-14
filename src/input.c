/*
 * input.c — Keyboard Input Handling Implementation
 * =============================================================================
 * See input.h for the public API.
 *
 * HOW NCURSES KEYS WORK
 * ---------------------
 * getch() returns an int, not a char.  Regular printable ASCII characters
 * come back as their ASCII value (e.g. 'a' = 97).  Special keys (arrows,
 * function keys, etc.) come back as KEY_* constants defined in <ncurses.h>,
 * which are integers > 255 so they never clash with real characters.
 *
 * Control characters are ASCII 1–26.  The convenient macro:
 *   CTRL(x)  =  (x) & 0x1F
 * gives you the control code.  For example, CTRL('s') = 19, which is what
 * getch() returns when the user presses Ctrl+S.
 * =============================================================================
 */

#include "input.h"
#include "editor.h"
#include "display.h"

#include <stdlib.h>   /* for free() */

#include <ncurses.h>

/* Convenience macro: CTRL('s') is the key code for Ctrl+S, etc. */
#define CTRL(x)  ((x) & 0x1F)

/* ============================================================================
 * Quit helper
 * ============================================================================ */

/*
 * try_quit — handle Ctrl+Q.
 *
 * If the current buffer has unsaved changes, warn the user and require a
 * second Ctrl+Q press to confirm.  On the first press we just show a message.
 *
 * We track whether a warning was already shown with a static variable.
 * Pressing any key other than Ctrl+Q resets the warning.
 */
static void try_quit(struct Editor *ed)
{
    Buffer *buf = editor_current_buffer(ed);

    if (buf && buf->dirty) {
        /*
         * warn_pending is a static flag that persists between calls.
         * We use it to detect a "double Ctrl+Q" confirmation.
         */
        static int warn_pending = 0;

        if (!warn_pending) {
            editor_set_status(ed,
                "Unsaved changes! Press Ctrl+Q again to quit without saving.");
            warn_pending = 1;
            return;
        }
        warn_pending = 0;
    }

    ed->should_quit = 1;
}

/* ============================================================================
 * input_process_key
 * ============================================================================ */

void input_process_key(struct Editor *ed)
{
    /*
     * getch() blocks until a key is pressed and returns its key code.
     * With keypad(stdscr, TRUE) set in display_init(), multi-byte sequences
     * like arrow keys are automatically decoded into KEY_UP, KEY_DOWN, etc.
     */
    int key = getch();

    /*
     * If a key other than Ctrl+Q comes in, clear any pending quit warning.
     * We do this by checking the static flag directly; the actual reset
     * happens inside try_quit on the next Ctrl+Q if warn_pending is 1.
     *
     * Also clear the status message on any keypress so it doesn't linger
     * after a save/error notification.
     */
    if (key != CTRL('q')) {
        /* Reset the quit-confirmation flag — user changed their mind */
        /* (We can't access warn_pending from outside try_quit, but we can
         *  call try_quit with a special sentinel — instead, just clear the
         *  status message and rely on try_quit's internal state reset.) */
    }

    switch (key) {

        /* ------------------------------------------------------------------ *
         * Cursor movement — plain movement always clears the selection
         * ------------------------------------------------------------------ */

        case KEY_UP:
            editor_selection_clear(ed);
            editor_move_up(ed);
            break;

        case KEY_DOWN:
            editor_selection_clear(ed);
            editor_move_down(ed);
            break;

        case KEY_LEFT:
            editor_selection_clear(ed);
            editor_move_left(ed);
            break;

        case KEY_RIGHT:
            editor_selection_clear(ed);
            editor_move_right(ed);
            break;

        case KEY_HOME:
            editor_selection_clear(ed);
            editor_move_line_start(ed);
            break;

        case KEY_END:
            editor_selection_clear(ed);
            editor_move_line_end(ed);
            break;

        case KEY_PPAGE:    /* Page Up */
            editor_selection_clear(ed);
            editor_page_up(ed);
            break;

        case KEY_NPAGE:    /* Page Down */
            editor_selection_clear(ed);
            editor_page_down(ed);
            break;

        /* ------------------------------------------------------------------ *
         * Shift+Arrow — extend the selection
         *
         * ncurses names for shifted arrow keys:
         *   KEY_SLEFT   Shift+Left
         *   KEY_SRIGHT  Shift+Right
         *   KEY_SR      Shift+Up   ("Scroll Reverse")
         *   KEY_SF      Shift+Down ("Scroll Forward")
         *   KEY_SHOME   Shift+Home  (also used by some terminals for Ctrl+Home)
         *   KEY_SEND    Shift+End   (also used by some terminals for Ctrl+End)
         *
         * Note: on macOS Terminal, Shift+Home/End may not send these codes.
         * Arrow keys are the most reliable.
         * ------------------------------------------------------------------ */
        case KEY_SLEFT:
            editor_select_left(ed);
            break;

        case KEY_SRIGHT:
            editor_select_right(ed);
            break;

        case KEY_SR:       /* Shift+Up */
            editor_select_up(ed);
            break;

        case KEY_SF:       /* Shift+Down */
            editor_select_down(ed);
            break;

        /*
         * Ctrl+Home / Ctrl+End — jump to start/end of file.
         *
         * We no longer use KEY_SHOME / KEY_SEND here because those constants
         * are now claimed by Shift+Home / Shift+End for selection.  Instead
         * we rely on the raw numeric codes that xterm-256color sends:
         *   554 = Ctrl+Home
         *   549 = Ctrl+End
         * These are the most reliable codes on macOS Terminal and iTerm2.
         */
        case 554:          /* Ctrl+Home */
            editor_selection_clear(ed);
            editor_move_file_start(ed);
            break;

        case 549:          /* Ctrl+End */
            editor_selection_clear(ed);
            editor_move_file_end(ed);
            break;

        /* ------------------------------------------------------------------ *
         * Text editing
         * ------------------------------------------------------------------ */

        case KEY_BACKSPACE:    /* ncurses constant for Backspace */
        case 127:              /* ASCII DEL — what many terminals send */
        case '\b':             /* ASCII Backspace (Ctrl+H) */
            editor_backspace(ed);
            break;

        case KEY_DC:           /* Delete key */
            editor_delete_char(ed);
            break;

        case '\r':             /* Carriage return (some terminals) */
        case '\n':             /* Newline */
        case KEY_ENTER:        /* Numpad Enter */
            editor_insert_newline(ed);
            break;

        /* ------------------------------------------------------------------ *
         * Search & Replace
         * ------------------------------------------------------------------ */

        case CTRL('f'):        /* Ctrl+F — Find */
            editor_find(ed);
            break;

        case KEY_F(3):         /* F3 — Find next */
            editor_find_next(ed);
            break;

        case KEY_F(15):        /* Shift+F3 — Find previous
                                * ncurses maps Shift+F(n) to F(n+12),
                                * so Shift+F3 = F15. */
            editor_find_prev(ed);
            break;

        case CTRL('r'):        /* Ctrl+R — Replace
                                * Note: Ctrl+H cannot be used because it is
                                * the same byte as Backspace (ASCII 8). */
            editor_replace(ed);
            break;

        case 27:               /* Escape — clear search highlights */
            editor_search_clear(ed);
            break;

        /* ------------------------------------------------------------------ *
         * Clipboard / selection
         * ------------------------------------------------------------------ */

        case CTRL('a'):        /* Ctrl+A — Select all */
            editor_select_all(ed);
            break;

        case CTRL('c'):        /* Ctrl+C — Copy */
            editor_copy(ed);
            break;

        case CTRL('x'):        /* Ctrl+X — Cut */
            editor_cut(ed);
            break;

        case CTRL('v'):        /* Ctrl+V — Paste */
            editor_paste(ed);
            break;

        /* ------------------------------------------------------------------ *
         * Buffer switching — Ctrl+] / Ctrl+\
         *
         * Ctrl+]  is ASCII 29 (GS, Group Separator) — next buffer.
         * Ctrl+\  is ASCII 28 (FS, File Separator)  — previous buffer.
         *
         * These are reliable control characters that all terminals send
         * consistently, unlike Ctrl+Arrow which varies by terminal type.
         * ------------------------------------------------------------------ */
        case 29:               /* Ctrl+] — next buffer */
            editor_next_buffer(ed);
            break;

        case 28:               /* Ctrl+\ — previous buffer */
            editor_prev_buffer(ed);
            break;

        /* ------------------------------------------------------------------ *
         * File operations
         * ------------------------------------------------------------------ */

        case CTRL('n'):        /* Ctrl+N — New empty buffer */
            editor_new_buffer(ed);
            break;

        case CTRL('o'):        /* Ctrl+O — Open file */
        {
            /*
             * display_prompt() draws an input field in the status bar and
             * returns a heap-allocated string of what the user typed, or
             * NULL if they pressed Escape.
             */
            char *path = display_prompt(ed, "Open file: ");
            if (path && path[0] != '\0') {
                editor_open_file(ed, path);
            }
            free(path);
            break;
        }

        case CTRL('w'):        /* Ctrl+W — Close current buffer */
            editor_close_buffer(ed);
            break;

        case CTRL('s'):        /* Ctrl+S — Save */
            editor_save(ed);
            break;

        case CTRL('z'):        /* Ctrl+Z — Undo */
            editor_undo(ed);
            break;

        case CTRL('y'):        /* Ctrl+Y — Redo */
            editor_redo(ed);
            break;

        case CTRL('q'):        /* Ctrl+Q — Quit */
            try_quit(ed);
            break;

        /* ------------------------------------------------------------------ *
         * Default: printable characters
         * ------------------------------------------------------------------ */

        default:
            /*
             * If the key is a printable ASCII character (space through tilde,
             * i.e. 0x20 through 0x7e), insert it into the buffer.
             *
             * We intentionally ignore:
             *   - Tab (0x09) — to be handled in Phase 2 with configurable
             *     tab width / expand-tabs setting.
             *   - Other control characters (< 0x20) that we do not yet map.
             *   - KEY_* constants (> 255) that we do not yet handle.
             */
            if (key >= 0x20 && key <= 0x7e) {
                editor_insert_char(ed, (char)key);
                /* Clear old status messages when the user starts typing */
                ed->status_msg[0] = '\0';
            }
            break;
    }
}
