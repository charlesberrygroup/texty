/*
 * gui.h — SDL2 GUI Frontend for Texty IDE
 * =============================================================================
 * Declares the entry point for the graphical user interface mode.
 * When the user runs `./texty -G`, main() calls gui_main() instead of
 * entering the ncurses-based TUI.
 *
 * The GUI uses SDL2 for window management and event handling, and
 * SDL2_ttf for text rendering with a monospace font.  All editor logic
 * (buffer, syntax, git, build, finder, undo, LSP, etc.) is shared with
 * the TUI — only the display and input layers are different.
 *
 * Architecture:
 *   gui_main() creates an SDL window and enters its own event loop.
 *   Keyboard events are translated from SDL keycodes into the ncurses
 *   KEY_* constants that input_process_key_with() expects, so all the
 *   existing key binding logic in input.c is reused.
 *
 *   For functions that editor.c calls back into the display layer
 *   (display_prompt, display_finder_popup, display_apply_theme),
 *   gui_main() installs override function pointers via setters in
 *   display.h so the calls are redirected to the GUI implementations.
 *
 * Dependencies:
 *   - SDL2         (window, renderer, events)
 *   - SDL2_ttf     (TrueType font rendering)
 * =============================================================================
 */

#ifndef GUI_H
#define GUI_H

/**
 * gui_main — entry point for the SDL2 GUI.
 *
 * Sets up the SDL window, loads a monospace font, installs display
 * overrides, and runs the graphical event loop.  Reuses the same
 * Editor, Buffer, and all other logic modules as the TUI.
 *
 * argc/argv are the original command-line arguments (the -G flag is
 * still present; gui_main skips it when scanning for filenames).
 *
 * Returns EXIT_SUCCESS or EXIT_FAILURE.
 */
int gui_main(int argc, char *argv[]);

#endif /* GUI_H */
