/*
 * display_stub.c — Fake display functions for unit tests
 * =============================================================================
 * editor.c calls display_prompt() for interactive search/replace prompts.
 * In tests we have no terminal or ncurses, so we provide stub implementations
 * that satisfy the linker without doing anything real.
 *
 * This file is compiled into every test binary that links editor.o.
 * =============================================================================
 */

#include "display.h"
#include <stdlib.h>   /* NULL */

/*
 * display_prompt — in tests, always return NULL (simulates user pressing Escape).
 *
 * This means editor_find() and editor_replace() will see a cancelled prompt
 * and do nothing, which is fine — we test those operations at the buffer level
 * rather than through the interactive prompt path.
 */
char *display_prompt(struct Editor *ed, const char *prompt)
{
    (void)ed;      /* suppress unused-parameter warning */
    (void)prompt;
    return NULL;
}

/*
 * display_render — no-op in tests (no terminal to draw to).
 *
 * editor_build() calls display_render() to flush "Building..." to screen
 * before the blocking popen call.  In tests, we just do nothing.
 */
void display_render(struct Editor *ed)
{
    (void)ed;
}

/*
 * display_update_size — no-op in tests (no terminal dimensions).
 */
void display_update_size(struct Editor *ed)
{
    (void)ed;
}
