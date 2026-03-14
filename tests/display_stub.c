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
