/*
 * main.c — Texty IDE — Entry Point
 * =============================================================================
 *
 * Texty is a cross-platform terminal IDE written in C.
 * This file contains the main() function and the top-level event loop.
 *
 * Architecture overview
 * ---------------------
 *
 *   main.c          You are here.  Wires everything together.
 *   buffer.h/.c     Text storage — the in-memory representation of a file.
 *   editor.h/.c     Editor state (cursor, viewport, open files) and operations.
 *   display.h/.c    Terminal rendering via ncurses.
 *   input.h/.c      Keyboard input handling.
 *
 * Data flow
 * ---------
 *
 *   1. main() initialises the Editor struct and the ncurses display.
 *   2. If a filename was given on the command line, the file is opened.
 *   3. The main loop runs:
 *        a. display_render() — draws the current editor state on screen.
 *        b. input_process_key() — waits for a keypress and updates state.
 *      The loop exits when ed.should_quit is set to non-zero.
 *   4. Cleanup: ncurses is shut down, editor memory is freed.
 *
 * Building
 * --------
 *   make          — build the editor (outputs ./texty)
 *   make run      — build and run
 *   make clean    — remove build artifacts
 *
 * Usage
 * -----
 *   ./texty                  — open with a new empty buffer
 *   ./texty myfile.txt       — open a specific file
 *
 * Key bindings (Phase 1)
 * ----------------------
 *   Arrow keys / Home / End / PgUp / PgDn  — navigation
 *   Ctrl+Home / Ctrl+End                   — jump to top / bottom of file
 *   Backspace / Delete                     — delete character
 *   Enter                                  — insert newline
 *   Ctrl+S                                 — save
 *   Ctrl+Q                                 — quit (double-press if unsaved)
 *
 * =============================================================================
 */

#include "editor.h"
#include "display.h"
#include "input.h"

#include <stdio.h>
#include <stdlib.h>

int main(int argc, char *argv[])
{
    /* ---------------------------------------------------------------------- *
     * 1. Initialise the editor state
     * ---------------------------------------------------------------------- */
    Editor ed;
    editor_init(&ed);

    /* ---------------------------------------------------------------------- *
     * 2. Start ncurses and learn the terminal size
     * ---------------------------------------------------------------------- *
     * display_init() must be called before any other display or input
     * function because it sets up the ncurses environment that getch() and
     * the rendering functions depend on.
     */
    display_init();
    display_update_size(&ed);

    /* ---------------------------------------------------------------------- *
     * 3. Open a file or create an empty buffer
     * ---------------------------------------------------------------------- */
    if (argc >= 2) {
        /*
         * A filename was provided on the command line.
         * editor_open_file handles both existing files (loads them) and
         * new files (creates an empty buffer with the given name).
         */
        if (editor_open_file(&ed, argv[1]) != 0) {
            display_cleanup();
            fprintf(stderr, "texty: cannot open '%s'\n", argv[1]);
            return EXIT_FAILURE;
        }
    } else {
        /* No filename — start with a fresh, unnamed empty buffer */
        if (editor_new_buffer(&ed) != 0) {
            display_cleanup();
            fprintf(stderr, "texty: failed to create initial buffer\n");
            return EXIT_FAILURE;
        }
    }

    /* ---------------------------------------------------------------------- *
     * 4. Main event loop
     * ---------------------------------------------------------------------- *
     * Each iteration:
     *   - Renders the current state to the terminal.
     *   - Waits for (and processes) one keypress.
     *
     * The loop exits when any input handler sets ed.should_quit = 1
     * (for example, Ctrl+Q).
     */
    while (!ed.should_quit) {
        display_render(&ed);
        input_process_key(&ed);
    }

    /* ---------------------------------------------------------------------- *
     * 5. Cleanup
     * ---------------------------------------------------------------------- *
     * Order matters: shut down ncurses first so the terminal is back in
     * normal mode, THEN free memory.
     */
    display_cleanup();
    editor_cleanup(&ed);

    return EXIT_SUCCESS;
}
