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
#include "theme.h"
#include "lsp.h"

#ifdef HAS_GUI
#include "gui.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <locale.h>    /* for setlocale — required for UTF-8 support in ncurses */
#include <termios.h>   /* for tcgetattr/tcsetattr — disable ISIG after halfdelay */
#include <unistd.h>    /* for STDIN_FILENO */
#include <ncurses.h>   /* for halfdelay, getch, ERR */

int main(int argc, char *argv[])
{
    /* ---------------------------------------------------------------------- *
     * Set locale for UTF-8 support
     *
     * ncurses needs to know the terminal's character encoding to correctly
     * handle multi-byte UTF-8 characters (accented letters, CJK, symbols).
     * Without this call, ncurses assumes single-byte characters and renders
     * multi-byte UTF-8 as boxes or question marks.
     *
     * setlocale(LC_ALL, "") reads the user's locale from environment
     * variables (LANG, LC_ALL).  On modern systems this is typically
     * "en_US.UTF-8" or similar.  Must be called BEFORE initscr().
     * ---------------------------------------------------------------------- */
    setlocale(LC_ALL, "");

    /* ---------------------------------------------------------------------- *
     * Check for -G flag (GUI mode)
     *
     * If the user runs `./texty -G`, we launch the SDL2 graphical frontend
     * instead of the ncurses terminal UI.  The -G flag can appear anywhere
     * in the argument list (before or after a filename).
     * ---------------------------------------------------------------------- */
#ifdef HAS_GUI
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-G") == 0) {
            return gui_main(argc, argv);
        }
    }
#else
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-G") == 0) {
            fprintf(stderr, "texty: GUI mode requires SDL2 and SDL2_ttf.\n");
            fprintf(stderr, "  Install them and rebuild:\n");
            fprintf(stderr, "    brew install sdl2 sdl2_ttf   (macOS)\n");
            fprintf(stderr, "    sudo apt install libsdl2-dev libsdl2-ttf-dev   (Linux)\n");
            return EXIT_FAILURE;
        }
    }
#endif

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

    /*
     * Apply the active theme.  theme_init() was called inside editor_init()
     * which loaded built-in themes and user themes from disk.  Now we
     * re-register all ncurses color pairs to match the active theme.
     * This overrides the defaults set in display_init().
     */
    display_apply_theme(theme_active(&ed.theme_mgr));

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
     * 4. Try to start LSP for the initial file
     * ---------------------------------------------------------------------- */
    editor_lsp_start(&ed);

    /* ---------------------------------------------------------------------- *
     * 5. Main event loop
     * ---------------------------------------------------------------------- *
     * The event loop uses halfdelay(1) so getch() returns ERR after 100ms
     * if no key is pressed.  This allows us to periodically check for
     * incoming LSP messages without blocking forever on keyboard input.
     *
     * Each iteration:
     *   1. Render the current state.
     *   2. Wait up to 100ms for a keypress.
     *   3. If a key was pressed, process it via input_process_key().
     *   4. Poll the LSP server for incoming messages (non-blocking).
     *
     * The loop exits when any input handler sets ed.should_quit = 1.
     */
    halfdelay(1);  /* getch() returns ERR after 100ms (1 tenth of a second) */

    /*
     * halfdelay() internally uses cbreak() mode, which RE-ENABLES the
     * terminal's ISIG flag.  This undoes raw()'s signal suppression:
     * Ctrl+C would generate SIGINT (killing the editor) instead of
     * passing character 0x03 to getch().
     *
     * Fix: clear ISIG from the terminal attributes after halfdelay().
     * This preserves the 100ms timeout but prevents signal generation,
     * so Ctrl+C reaches getch() as a normal keypress (code 3 = copy).
     */
    {
        struct termios t;
        tcgetattr(STDIN_FILENO, &t);
        t.c_lflag &= ~ISIG;
        tcsetattr(STDIN_FILENO, TCSANOW, &t);
    }

    while (!ed.should_quit) {
        display_render(&ed);

        int key = getch();
        if (key != ERR)
            input_process_key_with(&ed, key);

        /* Check for incoming LSP messages (diagnostics, responses, etc.) */
        editor_lsp_poll(&ed);
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
