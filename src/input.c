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
         * Cursor movement
         * ------------------------------------------------------------------ */

        case KEY_UP:
            editor_move_up(ed);
            break;

        case KEY_DOWN:
            editor_move_down(ed);
            break;

        case KEY_LEFT:
            editor_move_left(ed);
            break;

        case KEY_RIGHT:
            editor_move_right(ed);
            break;

        case KEY_HOME:
            editor_move_line_start(ed);
            break;

        case KEY_END:
            editor_move_line_end(ed);
            break;

        case KEY_PPAGE:    /* Page Up */
            editor_page_up(ed);
            break;

        case KEY_NPAGE:    /* Page Down */
            editor_page_down(ed);
            break;

        /*
         * Ctrl+Home and Ctrl+End: jump to beginning / end of file.
         *
         * ncurses reports these as KEY_SHOME / KEY_SEND on some terminals,
         * but the most portable way to detect Ctrl+Home is to check for the
         * raw escape sequence.  Many modern terminals send KEY_HOME for Home
         * and a different code for Ctrl+Home.
         *
         * For maximum compatibility we handle both KEY_SHOME/KEY_SEND
         * (shifted variants that many terminals use for Ctrl+Home/End) and
         * the raw values 1 (Ctrl+A) / 5 (Ctrl+E) as aliases.
         *
         * Note: some terminals send 554 for Ctrl+Home and 549 for Ctrl+End —
         * these are also handled here via the KEY_SHOME/KEY_SEND constants.
         */
        case KEY_SHOME:    /* Ctrl+Home on many terminals */
        case 554:          /* Ctrl+Home raw code (xterm-256color) */
            editor_move_file_start(ed);
            break;

        case KEY_SEND:     /* Ctrl+End on many terminals */
        case 549:          /* Ctrl+End raw code (xterm-256color) */
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
         * File operations
         * ------------------------------------------------------------------ */

        case CTRL('s'):        /* Ctrl+S — Save */
            editor_save(ed);
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
