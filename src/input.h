/*
 * input.h — Keyboard Input Handling
 * =============================================================================
 * Reads a single key event from ncurses and dispatches it to the correct
 * editor action.
 *
 * Key binding summary (Phase 1)
 * -----------------------------
 *   Arrow keys            Move cursor
 *   Home / End            Start / end of line
 *   Page Up / Page Down   Scroll one screen
 *   Ctrl+Home             Jump to start of file
 *   Ctrl+End              Jump to end of file
 *   Backspace             Delete character before cursor
 *   Delete                Delete character under cursor
 *   Enter                 Insert newline
 *   Ctrl+S                Save file
 *   Ctrl+Q                Quit (warns on unsaved changes)
 *   All printable chars   Insert into buffer
 * =============================================================================
 */

#ifndef INPUT_H
#define INPUT_H

/* Forward declaration */
struct Editor;

/**
 * input_process_key — block until a key is pressed, then act on it.
 *
 * This function:
 *   1. Calls getch() to wait for a keypress.
 *   2. Maps the raw key code to an editor operation.
 *   3. Calls the appropriate editor_* function.
 *
 * It is the main "glue" between user input and editor state changes.
 */
void input_process_key(struct Editor *ed);

/**
 * input_process_key_with — process a pre-read key code.
 *
 * Like input_process_key(), but takes a key that was already read by
 * the caller (via getch()).  Used by the halfdelay-based event loop
 * where the main loop reads the key and only calls this if key != ERR.
 */
void input_process_key_with(struct Editor *ed, int key);

#endif /* INPUT_H */
