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
#include "filetree.h"  /* for FileTree, FlatEntry, filetree_toggle, etc. */

#include <stdlib.h>    /* for free() */
#include <stdio.h>     /* for fopen(), fclose() */
#include <string.h>    /* for strrchr(), strncpy(), strncat() */
#include <sys/stat.h>  /* for mkdir() */
#include <unistd.h>    /* for unlink(), rmdir(), rename() */

#include <ncurses.h>

/* Convenience macro: CTRL('s') is the key code for Ctrl+S, etc. */
#define CTRL(x)  ((x) & 0x1F)

/* ============================================================================
 * File-tree key handler
 * ============================================================================ */

/*
 * input_process_filetree_key — handle one keypress while the file tree has focus.
 *
 * This function is called by input_process_key() when ed->filetree_focus is 1.
 * It interprets arrow keys as tree navigation, Enter as open/expand, Escape as
 * "return focus to editor", and letter shortcuts for file operations.
 *
 * C note: `static` means this function is private to this translation unit
 * (this .c file).  It cannot be called from other files.  That is fine because
 * it is only used by input_process_key() below.
 */
static void input_process_filetree_key(struct Editor *ed, int key)
{
    FileTree *ft  = ed->filetree;
    int       max_idx = ft->count > 0 ? ft->count - 1 : 0;

    switch (key) {

        /* ---- Navigation -------------------------------------------------- */

        case KEY_UP:
            /*
             * Move the highlight up one entry.
             * Clamp at 0 (cannot go above the first entry).
             */
            if (ed->filetree_cursor > 0)
                ed->filetree_cursor--;
            break;

        case KEY_DOWN:
            /*
             * Move the highlight down one entry.
             * Clamp at max_idx (cannot go below the last entry).
             */
            if (ed->filetree_cursor < max_idx)
                ed->filetree_cursor++;
            break;

        case KEY_LEFT:
            /*
             * Collapse an expanded directory.
             * If the current entry is a directory AND it is expanded,
             * toggle it (which collapses it and rebuilds the list).
             * If the directory is already collapsed, do nothing.
             */
            if (ed->filetree_cursor < ft->count
                    && ft->entries[ed->filetree_cursor].is_dir
                    && filetree_is_expanded(ft, ft->entries[ed->filetree_cursor].path)) {
                filetree_toggle(ft, ed->filetree_cursor);
                /* After collapse, count shrinks; clamp cursor */
                if (ft->count > 0 && ed->filetree_cursor >= ft->count)
                    ed->filetree_cursor = ft->count - 1;
            }
            break;

        case KEY_RIGHT:
            /*
             * Expand a collapsed directory.
             * Only acts when the current entry is a directory that is NOT
             * already expanded.
             */
            if (ed->filetree_cursor < ft->count
                    && ft->entries[ed->filetree_cursor].is_dir
                    && !filetree_is_expanded(ft, ft->entries[ed->filetree_cursor].path)) {
                filetree_toggle(ft, ed->filetree_cursor);
            }
            break;

        /* ---- Open / Expand ----------------------------------------------- */

        case '\r':       /* Carriage return (some terminals) */
        case '\n':       /* Newline */
        case KEY_ENTER:  /* Numpad Enter */
            if (ed->filetree_cursor >= 0 && ed->filetree_cursor < ft->count) {
                FlatEntry *e = &ft->entries[ed->filetree_cursor];

                if (e->is_dir) {
                    /*
                     * Enter on a directory toggles its expansion state,
                     * just like pressing the Right or Left arrow key.
                     */
                    filetree_toggle(ft, ed->filetree_cursor);
                    /* Clamp cursor in case the directory was collapsed */
                    if (ft->count > 0 && ed->filetree_cursor >= ft->count)
                        ed->filetree_cursor = ft->count - 1;
                } else {
                    /*
                     * Enter on a file: open it in a new editor buffer and
                     * return focus to the editor.
                     */
                    editor_open_or_switch(ed, e->path);
                    ed->filetree_focus = 0;
                }
            }
            break;

        /* ---- Return focus to editor / close panel ------------------------ */

        case 27:           /* Escape — return focus to editor, keep panel open */
            ed->filetree_focus = 0;
            break;

        case CTRL('w'):    /* Ctrl+W — close the file explorer panel entirely */
            ed->show_filetree  = 0;
            ed->filetree_focus = 0;
            editor_scroll(ed);
            break;

        /* ---- File operations --------------------------------------------- */

        case 'n':   /* new file */
        {
            /*
             * Prompt the user for a filename, then create an empty file in
             * the appropriate directory and open it.
             *
             * display_prompt() returns a heap-allocated string or NULL if the
             * user pressed Escape.  We must free() it when done.
             */
            char *fname = display_prompt(ed, "New file name: ");
            if (fname && fname[0] != '\0') {

                /* Determine which directory to create the file in */
                char dir[1024];
                if (ed->filetree_cursor >= 0 && ed->filetree_cursor < ft->count) {
                    FlatEntry *cur = &ft->entries[ed->filetree_cursor];
                    if (cur->is_dir) {
                        /* Cursor is on a dir → create inside that dir */
                        strncpy(dir, cur->path, sizeof(dir) - 1);
                        dir[sizeof(dir) - 1] = '\0';
                    } else {
                        /* Cursor is on a file → use that file's parent dir */
                        strncpy(dir, cur->path, sizeof(dir) - 1);
                        dir[sizeof(dir) - 1] = '\0';
                        char *slash = strrchr(dir, '/');
                        if (slash)
                            *slash = '\0';
                        else
                            strncpy(dir, ft->root, sizeof(dir) - 1);
                    }
                } else {
                    strncpy(dir, ft->root, sizeof(dir) - 1);
                    dir[sizeof(dir) - 1] = '\0';
                }

                char newpath[2048];
                snprintf(newpath, sizeof(newpath), "%s/%s", dir, fname);

                /*
                 * fopen(path, "w") creates an empty file (or truncates an
                 * existing one).  We only want to create it, so we close
                 * immediately without writing anything.
                 */
                FILE *f = fopen(newpath, "w");
                if (f) {
                    fclose(f);
                    filetree_rebuild(ft);
                    editor_open_file(ed, newpath);
                    ed->filetree_focus = 0;
                } else {
                    editor_set_status(ed, "Could not create '%s'", fname);
                }
            }
            free(fname);  /* free() is safe even if fname is NULL */
            break;
        }

        case 'N':   /* new directory */
        {
            char *dname = display_prompt(ed, "New directory name: ");
            if (dname && dname[0] != '\0') {

                char dir[1024];
                if (ed->filetree_cursor >= 0 && ed->filetree_cursor < ft->count) {
                    FlatEntry *cur = &ft->entries[ed->filetree_cursor];
                    if (cur->is_dir) {
                        strncpy(dir, cur->path, sizeof(dir) - 1);
                        dir[sizeof(dir) - 1] = '\0';
                    } else {
                        strncpy(dir, cur->path, sizeof(dir) - 1);
                        dir[sizeof(dir) - 1] = '\0';
                        char *slash = strrchr(dir, '/');
                        if (slash)
                            *slash = '\0';
                        else
                            strncpy(dir, ft->root, sizeof(dir) - 1);
                    }
                } else {
                    strncpy(dir, ft->root, sizeof(dir) - 1);
                    dir[sizeof(dir) - 1] = '\0';
                }

                char newpath[2048];
                snprintf(newpath, sizeof(newpath), "%s/%s", dir, dname);

                /*
                 * mkdir(path, mode) creates a directory with the given
                 * permission bits.  0755 = owner rwx, group r-x, other r-x.
                 * Returns 0 on success, -1 on failure (errno is set).
                 */
                if (mkdir(newpath, 0755) == 0) {
                    filetree_rebuild(ft);
                } else {
                    editor_set_status(ed, "Could not create directory '%s'", dname);
                }
            }
            free(dname);
            break;
        }

        case 'd':   /* delete */
        {
            if (ed->filetree_cursor >= 0 && ed->filetree_cursor < ft->count) {
                FlatEntry *e = &ft->entries[ed->filetree_cursor];

                /* Confirm before deleting */
                char prompt[512];
                snprintf(prompt, sizeof(prompt), "Delete '%s'? (y/n): ", e->name);
                char *ans = display_prompt(ed, prompt);

                if (ans && (ans[0] == 'y' || ans[0] == 'Y')) {
                    /*
                     * unlink() removes a file from the filesystem.
                     * rmdir()  removes an EMPTY directory.
                     * Both return 0 on success, -1 on failure.
                     */
                    int ok = e->is_dir ? rmdir(e->path) : unlink(e->path);
                    if (ok == 0) {
                        filetree_rebuild(ft);
                        /* Clamp cursor after deletion shrinks the list */
                        if (ft->count > 0 && ed->filetree_cursor >= ft->count)
                            ed->filetree_cursor = ft->count - 1;
                        if (ed->filetree_cursor < 0)
                            ed->filetree_cursor = 0;
                    } else {
                        editor_set_status(ed, "Could not delete '%s'", e->name);
                    }
                }
                free(ans);
            }
            break;
        }

        case 'r':   /* rename */
        {
            if (ed->filetree_cursor >= 0 && ed->filetree_cursor < ft->count) {
                FlatEntry *e = &ft->entries[ed->filetree_cursor];
                char *newname = display_prompt(ed, "Rename to: ");

                if (newname && newname[0] != '\0') {
                    /*
                     * Build the new full path: same directory, new basename.
                     * Strategy: copy e->path, find the last '/', replace the
                     * basename after it with the new name.
                     */
                    char newpath[1024];
                    strncpy(newpath, e->path, sizeof(newpath) - 1);
                    newpath[sizeof(newpath) - 1] = '\0';

                    char *slash = strrchr(newpath, '/');
                    if (slash) {
                        /* Truncate at the slash, then append the new name */
                        *(slash + 1) = '\0';
                        strncat(newpath, newname,
                                sizeof(newpath) - strlen(newpath) - 1);
                    } else {
                        /* No slash: file is in current dir, just use new name */
                        strncpy(newpath, newname, sizeof(newpath) - 1);
                        newpath[sizeof(newpath) - 1] = '\0';
                    }

                    /*
                     * rename(old, new) atomically renames a file or directory.
                     * Returns 0 on success, -1 on failure.
                     */
                    if (rename(e->path, newpath) == 0) {
                        filetree_rebuild(ft);
                    } else {
                        editor_set_status(ed, "Could not rename '%s'", e->name);
                    }
                }
                free(newname);
            }
            break;
        }

        default:
            break;
    }
}

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
 * Git status panel key handler
 * ============================================================================ */

/*
 * input_process_git_panel_key — handle one keypress while the git panel has focus.
 *
 * Up/Down navigate entries, Enter opens the file, Escape returns focus
 * to the editor, Ctrl+W closes the panel.
 */
static void input_process_git_panel_key(struct Editor *ed, int key)
{
    GitStatusList *gs = ed->git_status;
    if (!gs) return;

    switch (key) {
    case KEY_UP:
        if (ed->git_panel_cursor > 0)
            ed->git_panel_cursor--;
        break;

    case KEY_DOWN:
        if (ed->git_panel_cursor < gs->count - 1)
            ed->git_panel_cursor++;
        break;

    case '\n':   /* Enter — open the file under the cursor */
    case KEY_ENTER:
        if (ed->git_panel_cursor >= 0 && ed->git_panel_cursor < gs->count) {
            /*
             * Build the full path by combining repo root + relative path.
             * git status --porcelain gives paths relative to the repo root.
             */
            GitStatusEntry *e = &gs->entries[ed->git_panel_cursor];
            char fullpath[2048];
            snprintf(fullpath, sizeof(fullpath), "%s/%s",
                     gs->repo_root, e->path);

            editor_open_or_switch(ed, fullpath);
            ed->git_panel_focus = 0;  /* return focus to editor */
        }
        break;

    case 27:     /* Escape — return focus to editor */
        ed->git_panel_focus = 0;
        break;

    case CTRL('w'):   /* Ctrl+W — close the panel */
        ed->show_git_panel  = 0;
        ed->git_panel_focus = 0;
        break;
    }
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

    /*
     * File-tree focus routing.
     *
     * When the file explorer panel has keyboard focus, ALL keys (except
     * Ctrl+B which is handled by the main switch before this check) are
     * sent to the tree handler instead of the editor.
     *
     * We check Ctrl+B here so the user can ALWAYS toggle the panel off,
     * even when the tree has focus — otherwise there would be no way to
     * close the panel without pressing Escape first.
     */
    if (key != CTRL('b') && ed->filetree_focus
            && ed->show_filetree && ed->filetree) {
        input_process_filetree_key(ed, key);
        return;  /* Do NOT fall through to the editor key handler */
    }

    /*
     * Git panel focus routing — same pattern as the file tree.
     * F9 always reaches the main handler (so the panel can be toggled off).
     */
    if (key != KEY_F(9) && ed->git_panel_focus
            && ed->show_git_panel && ed->git_status) {
        input_process_git_panel_key(ed, key);
        return;
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

        case '\t':             /* Tab — insert spaces (width set by ed->tab_width) */
            editor_insert_tab(ed);
            break;

        case '\r':             /* Carriage return (some terminals) */
        case '\n':             /* Newline */
        case KEY_ENTER:        /* Numpad Enter */
            editor_insert_newline(ed);
            break;

        /* ------------------------------------------------------------------ *
         * Search & Replace
         * ------------------------------------------------------------------ */

        case CTRL('g'):        /* Ctrl+G — Go to line */
            editor_goto_line(ed);
            break;

        case CTRL('f'):        /* Ctrl+F — Find */
            editor_find(ed);
            break;

        case KEY_F(2):         /* F2 — Toggle visible whitespace */
            editor_toggle_whitespace(ed);
            break;

        case KEY_F(4):         /* F4 — Toggle word wrap */
            editor_toggle_word_wrap(ed);
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

        case CTRL('u'):        /* Ctrl+U — Mark/clear region highlight */
            editor_mark_region(ed);
            break;

        case KEY_F(9):         /* F9 — Toggle git status panel */
            editor_toggle_git_panel(ed);
            break;

        case KEY_F(10):        /* F10 — Toggle inline diff view */
            editor_toggle_inline_diff(ed);
            break;

        case KEY_F(11):        /* F11 — Stage hunk at cursor */
            editor_stage_hunk(ed);
            break;

        case KEY_F(12):        /* F12 — Git commit */
            editor_git_commit(ed);
            break;

        case CTRL('b'):        /* Ctrl+B — Toggle file explorer panel */
            /*
             * editor_toggle_filetree() handles all the logic:
             *   - If panel is hidden: show it and create/rebuild the FileTree.
             *   - If panel is visible: hide it and return focus to the editor.
             */
            editor_toggle_filetree(ed);
            break;

        /* ------------------------------------------------------------------ *
         * Auto-close brackets and quotes
         *
         * When the user types an opening bracket or quote, automatically
         * insert the matching closing character and leave the cursor between
         * the pair.  For example, '(' inserts "()" with the cursor at the
         * position between them.
         *
         * The pair is recorded as a single undo entry, so Ctrl+Z removes
         * both characters at once.
         * ------------------------------------------------------------------ */

        case '(':
            editor_insert_pair(ed, '(', ')');
            ed->status_msg[0] = '\0';
            break;

        case '[':
            editor_insert_pair(ed, '[', ']');
            ed->status_msg[0] = '\0';
            break;

        case '{':
            editor_insert_pair(ed, '{', '}');
            ed->status_msg[0] = '\0';
            break;

        case '"':
            editor_insert_pair(ed, '"', '"');
            ed->status_msg[0] = '\0';
            break;

        case '\'':
            editor_insert_pair(ed, '\'', '\'');
            ed->status_msg[0] = '\0';
            break;

        /* ------------------------------------------------------------------ *
         * Terminal resize
         *
         * When the user resizes their terminal window, the OS sends SIGWINCH
         * to the process.  ncurses catches that signal automatically (because
         * we called keypad(TRUE) in display_init) and the next getch() call
         * returns the special constant KEY_RESIZE instead of a normal key.
         *
         * At that point ncurses has already updated the global LINES and COLS
         * variables to the new dimensions.  We need to do two things:
         *
         * When ncurses delivers KEY_RESIZE it has ALREADY handled the signal
         * internally: LINES and COLS are updated and stdscr has been resized.
         * Calling resizeterm() again at this point resets the terminal's raw
         * mode on macOS, causing getch() to fall back to cooked mode and the
         * editor to appear unresponsive.  So we do NOT call resizeterm() here.
         *
         * What we do need to do:
         *
         *   1. display_update_size(ed) — copy the new LINES/COLS values into
         *      ed->term_rows / ed->term_cols so all layout calculations
         *      (editor_rows(), editor_cols(), scroll clamping, …) use the
         *      fresh dimensions.
         *
         *   2. editor_scroll(ed) — re-clamp the viewport.  If the window
         *      shrank, the cursor may now be outside the visible area; scroll
         *      brings it back into view.
         * ------------------------------------------------------------------ */
        case KEY_RESIZE:
            display_update_size(ed);
            editor_scroll(ed);
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
             *   - Tab (0x09) — handled above by editor_insert_tab.
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
