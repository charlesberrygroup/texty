/*
 * gui.c — SDL2 GUI Frontend Implementation
 * =============================================================================
 * This file implements the complete graphical user interface for texty.
 * It replaces display.c and input.c when running in GUI mode (-G flag).
 *
 * ARCHITECTURE OVERVIEW
 * ---------------------
 * The GUI reuses ALL logic modules (editor, buffer, syntax, git, build,
 * finder, filetree, theme, undo, lsp).  Only the rendering (ncurses → SDL2)
 * and input handling (getch → SDL events) are different.
 *
 * Keyboard input is translated from SDL keycodes to ncurses KEY_* constants
 * and passed to input_process_key_with(), so all key bindings work identically
 * to the TUI.
 *
 * For modal dialogs (prompts and the fuzzy finder), the GUI runs nested
 * SDL event loops — the same blocking pattern the TUI uses with getch().
 *
 * TEXT RENDERING
 * --------------
 * Text is rendered using SDL2_ttf (TTF_RenderText_Blended) in runs of
 * characters that share the same syntax color.  A monospace font ensures
 * every character occupies the same pixel width, making column calculations
 * trivial: pixel_x = column * char_width.
 *
 * COLOR MAPPING
 * -------------
 * The theme system defines colors as ncurses integers (0-7 for the 8 ANSI
 * colors, -1 for "terminal default").  The GUI maps these to RGB values
 * using a lookup table.  When the theme changes (F6), the cached RGB
 * values are rebuilt.
 * =============================================================================
 */

/* Only compile this file when SDL2 GUI support is available */
#ifdef HAS_GUI

#include <SDL.h>
#include <SDL_ttf.h>

#include "gui.h"
#include "editor.h"
#include "buffer.h"
#include "syntax.h"
#include "filetree.h"
#include "git.h"
#include "build.h"
#include "finder.h"
#include "theme.h"
#include "display.h"   /* for CPAIR_*, GUTTER_WIDTH, panel dimensions */
#include "input.h"     /* for input_process_key_with */
#include "lsp.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * On macOS, SDL redefines main via a macro.  We don't want that because
 * our real main() is in main.c — gui_main() is called from there.
 */
#ifdef main
#undef main
#endif

/* ============================================================================
 * Constants
 * ============================================================================ */

#define GUI_DEFAULT_WIDTH   1200   /* Initial window width in pixels           */
#define GUI_DEFAULT_HEIGHT  800    /* Initial window height in pixels          */
#define GUI_FONT_SIZE       15     /* Font size in points                     */
#define GUI_TAB_HEIGHT      28     /* Tab bar height in pixels                */
#define GUI_STATUS_HEIGHT   26     /* Status bar height in pixels             */
#define GUI_CURSOR_WIDTH    2      /* Cursor bar width in pixels              */

/* ============================================================================
 * Color Mapping — ncurses color integers (0-7) → RGB values
 *
 * These are the standard ANSI terminal color palette values.  The theme
 * system uses these integers, so we map them to RGB for SDL rendering.
 * ============================================================================ */

typedef struct { Uint8 r, g, b; } GuiRGB;

/*
 * Standard ANSI color palette.
 * Index 0-7 maps to COLOR_BLACK through COLOR_WHITE (the ncurses constants).
 */
static const GuiRGB ANSI_COLORS[8] = {
    {  0,   0,   0},    /* 0 = COLOR_BLACK   */
    {205,  49,  49},    /* 1 = COLOR_RED     */
    { 78, 154,   6},    /* 2 = COLOR_GREEN   */
    {196, 160,   0},    /* 3 = COLOR_YELLOW  */
    { 68, 113, 196},    /* 4 = COLOR_BLUE    */
    {170,  85, 162},    /* 5 = COLOR_MAGENTA */
    {  0, 175, 175},    /* 6 = COLOR_CYAN    */
    {211, 215, 207},    /* 7 = COLOR_WHITE   */
};

/* Defaults used when the theme specifies -1 (terminal default) */
static const GuiRGB DEFAULT_FG = {204, 204, 204};   /* light gray text   */
static const GuiRGB DEFAULT_BG = { 30,  30,  30};   /* dark gray bg      */

/* ============================================================================
 * GuiState — all SDL resources and cached state for the GUI
 * ============================================================================ */

typedef struct {
    SDL_Window   *window;
    SDL_Renderer *renderer;
    TTF_Font     *font;          /* regular weight monospace font             */
    TTF_Font     *font_bold;     /* bold weight (same font with TTF_STYLE)   */

    int           char_width;    /* pixel width of one monospace character    */
    int           char_height;   /* pixel height (line spacing)              */
    int           win_width;     /* current window width in pixels           */
    int           win_height;    /* current window height in pixels          */

    Editor       *editor;        /* pointer to the Editor struct             */

    /*
     * Cached theme colors — rebuilt when the theme changes.
     * Indexed by CPAIR_* constants (1-based).  Index 0 is unused.
     */
    GuiRGB        fg[THEME_NUM_PAIRS + 1];
    GuiRGB        bg[THEME_NUM_PAIRS + 1];
    int           bold[THEME_NUM_PAIRS + 1];

    /* The theme's default fg/bg (replaces -1 in color definitions) */
    GuiRGB        theme_fg;
    GuiRGB        theme_bg;
} GuiState;

/*
 * Global GuiState pointer.
 *
 * We use a global because the display_prompt/finder override callbacks
 * receive an Editor* but not a GuiState*.  Since there is only ever one
 * GUI instance, a global is the simplest solution.
 */
static GuiState *g_gui = NULL;

/* ============================================================================
 * Color Utilities
 * ============================================================================ */

/*
 * ncurses_to_rgb — convert an ncurses color integer to RGB.
 *
 * color: 0-7 for the ANSI palette, or -1 for "terminal default".
 * fallback: the RGB value to use when color is -1 (or out of range).
 */
static GuiRGB ncurses_to_rgb(short color, GuiRGB fallback)
{
    if (color >= 0 && color <= 7)
        return ANSI_COLORS[color];
    return fallback;
}

/*
 * gui_sdl_color — convert a GuiRGB to an SDL_Color (adds alpha = 255).
 */
static SDL_Color gui_sdl_color(GuiRGB c)
{
    SDL_Color sc = {c.r, c.g, c.b, 255};
    return sc;
}

/*
 * gui_cache_theme_colors — rebuild the cached fg/bg/bold arrays from the
 * current theme.  Called at startup and whenever the theme changes (F6).
 */
static void gui_cache_theme_colors(void)
{
    const Theme *t = theme_active(&g_gui->editor->theme_mgr);
    if (!t) return;

    /* Determine the base default colors from the theme */
    g_gui->theme_fg = ncurses_to_rgb(t->default_fg, DEFAULT_FG);
    g_gui->theme_bg = ncurses_to_rgb(t->default_bg, DEFAULT_BG);

    /* Convert each color pair definition */
    for (int i = 1; i <= THEME_NUM_PAIRS; i++) {
        g_gui->fg[i] = ncurses_to_rgb(t->pairs[i].fg, g_gui->theme_fg);
        g_gui->bg[i] = ncurses_to_rgb(t->pairs[i].bg, g_gui->theme_bg);
        g_gui->bold[i] = t->pairs[i].bold;
    }
}

/* ============================================================================
 * Font Loading
 *
 * Tries a list of well-known monospace font paths for macOS, Linux, and
 * Windows.  Uses the first one that succeeds.
 * ============================================================================ */

static const char *font_search_paths[] = {
    /* macOS */
    "/System/Library/Fonts/Menlo.ttc",
    "/System/Library/Fonts/Monaco.ttf",
    "/System/Library/Fonts/SFMono-Regular.otf",
    "/Library/Fonts/SF-Mono-Regular.otf",
    /* Linux */
    "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
    "/usr/share/fonts/truetype/liberation/LiberationMono-Regular.ttf",
    "/usr/share/fonts/truetype/ubuntu/UbuntuMono-R.ttf",
    "/usr/share/fonts/TTF/DejaVuSansMono.ttf",
    "/usr/share/fonts/dejavu-sans-mono-fonts/DejaVuSansMono.ttf",
    /* Windows */
    "C:\\Windows\\Fonts\\consola.ttf",
    "C:\\Windows\\Fonts\\cour.ttf",
    NULL
};

/*
 * gui_load_fonts — try to load a monospace font from well-known paths.
 *
 * Returns 0 on success (g_gui->font and font_bold are set),
 * -1 if no font could be found.
 */
static int gui_load_fonts(int size)
{
    for (int i = 0; font_search_paths[i]; i++) {
        g_gui->font = TTF_OpenFont(font_search_paths[i], size);
        if (g_gui->font) {
            /* Open the same font again for bold rendering */
            g_gui->font_bold = TTF_OpenFont(font_search_paths[i], size);
            if (g_gui->font_bold)
                TTF_SetFontStyle(g_gui->font_bold, TTF_STYLE_BOLD);
            else
                g_gui->font_bold = g_gui->font;  /* fallback: use regular */

            /*
             * Measure a single character to get the monospace cell size.
             * TTF_SizeText returns the pixel dimensions of a rendered string.
             * For a monospace font, every character has the same width.
             */
            TTF_SizeText(g_gui->font, "M", &g_gui->char_width, &g_gui->char_height);
            return 0;
        }
    }
    return -1;
}

/* ============================================================================
 * Drawing Primitives
 * ============================================================================ */

/*
 * gui_fill_rect — draw a filled rectangle.
 */
static void gui_fill_rect(int x, int y, int w, int h, GuiRGB c)
{
    SDL_SetRenderDrawColor(g_gui->renderer, c.r, c.g, c.b, 255);
    SDL_Rect r = {x, y, w, h};
    SDL_RenderFillRect(g_gui->renderer, &r);
}

/*
 * gui_draw_text — render a text string at pixel position (x, y).
 *
 * Uses TTF_RenderText_Blended for anti-aliased text.  Creates a temporary
 * SDL texture from the rendered surface, copies it to the renderer, then
 * frees both.  This is simple but creates/destroys textures per call.
 *
 * Returns the pixel width of the rendered text (useful for positioning
 * the next piece of text).
 */
static int gui_draw_text(const char *text, int x, int y,
                          SDL_Color color, TTF_Font *font)
{
    if (!text || text[0] == '\0') return 0;

    SDL_Surface *surf = TTF_RenderText_Blended(font, text, color);
    if (!surf) return 0;

    SDL_Texture *tex = SDL_CreateTextureFromSurface(g_gui->renderer, surf);
    if (!tex) {
        SDL_FreeSurface(surf);
        return 0;
    }

    SDL_Rect dst = {x, y, surf->w, surf->h};
    SDL_RenderCopy(g_gui->renderer, tex, NULL, &dst);

    int w = surf->w;
    SDL_DestroyTexture(tex);
    SDL_FreeSurface(surf);
    return w;
}

/*
 * gui_draw_char — render a single character at pixel position (x, y).
 */
static void gui_draw_char(char c, int x, int y,
                           SDL_Color color, TTF_Font *font)
{
    char buf[2] = {c, '\0'};
    gui_draw_text(buf, x, y, color, font);
}

/* ============================================================================
 * Terminal Size Calculation
 *
 * The editor uses term_rows and term_cols for scrolling and viewport
 * calculations.  In the TUI, these are the terminal dimensions.  In the
 * GUI, we calculate them from the window size and character dimensions.
 *
 * The editor expects:
 *   - Row 0 = tab bar
 *   - Rows 1..term_rows-2 = text area
 *   - Row term_rows-1 = status bar
 *
 * So: term_rows = (text_area_pixel_height / char_height) + 2
 * ============================================================================ */

static void gui_update_term_size(void)
{
    Editor *ed = g_gui->editor;

    /* Calculate pixel dimensions of the text area */
    int text_h = g_gui->win_height - GUI_TAB_HEIGHT - GUI_STATUS_HEIGHT;
    if (ed->show_build_panel)
        text_h -= BUILD_PANEL_HEIGHT * g_gui->char_height;
    if (text_h < g_gui->char_height)
        text_h = g_gui->char_height;

    int text_w = g_gui->win_width;
    if (ed->show_filetree)
        text_w -= FILETREE_WIDTH * g_gui->char_width;
    if (ed->show_git_panel)
        text_w -= GIT_PANEL_WIDTH * g_gui->char_width;
    if (text_w < g_gui->char_width)
        text_w = g_gui->char_width;

    /* Convert to character dimensions and set on the editor */
    int text_rows = text_h / g_gui->char_height;
    if (text_rows < 1) text_rows = 1;
    ed->term_rows = text_rows + 2;  /* +1 tab bar, +1 status bar */

    int text_cols = text_w / g_gui->char_width;
    if (text_cols < 1) text_cols = 1;
    ed->term_cols = text_cols;
}

/* ============================================================================
 * Rendering: Tab Bar
 * ============================================================================ */

static void gui_render_tab_bar(void)
{
    Editor *ed = g_gui->editor;
    int cw = g_gui->char_width;
    int w = g_gui->win_width;

    /* Background */
    gui_fill_rect(0, 0, w, GUI_TAB_HEIGHT, g_gui->bg[CPAIR_TAB_INACTIVE]);

    int x = 0;
    for (int i = 0; i < ed->num_buffers; i++) {
        Buffer *buf = ed->buffers[i];
        int active = (i == ed->current_buffer);

        /* Build tab label */
        const char *name = buf->filename ? strrchr(buf->filename, '/') : NULL;
        name = name ? name + 1 : (buf->filename ? buf->filename : "[No Name]");
        const char *dirt = buf->dirty ? " [+]" : "";

        char label[64];
        snprintf(label, sizeof(label), " %.20s%s ", name, dirt);
        int label_w = (int)strlen(label) * cw;

        if (x + label_w > w) break;

        /* Tab background */
        GuiRGB tab_bg = active ? g_gui->bg[CPAIR_TAB_ACTIVE]
                               : g_gui->bg[CPAIR_TAB_INACTIVE];
        GuiRGB tab_fg = active ? g_gui->fg[CPAIR_TAB_ACTIVE]
                               : g_gui->fg[CPAIR_TAB_INACTIVE];
        gui_fill_rect(x, 0, label_w, GUI_TAB_HEIGHT, tab_bg);

        /* Tab text — center vertically */
        int ty = (GUI_TAB_HEIGHT - g_gui->char_height) / 2;
        TTF_Font *f = (active && g_gui->bold[CPAIR_TAB_ACTIVE])
                    ? g_gui->font_bold : g_gui->font;
        gui_draw_text(label, x, ty, gui_sdl_color(tab_fg), f);

        x += label_w;

        /* Separator */
        if (i < ed->num_buffers - 1 && x < w) {
            gui_fill_rect(x, 0, 1, GUI_TAB_HEIGHT,
                          g_gui->fg[CPAIR_TAB_INACTIVE]);
            x += 1;
        }
    }
}

/* ============================================================================
 * Rendering: Status Bar
 * ============================================================================ */

static void gui_render_status_bar(void)
{
    Editor *ed = g_gui->editor;
    Buffer *buf = editor_current_buffer(ed);
    int y = g_gui->win_height - GUI_STATUS_HEIGHT;
    int w = g_gui->win_width;

    /* Choose clean/dirty status bar color */
    int cpair = (buf && buf->dirty) ? CPAIR_STATUS_DIRTY : CPAIR_STATUS;
    gui_fill_rect(0, y, w, GUI_STATUS_HEIGHT, g_gui->bg[cpair]);

    /* Build status text */
    char left[256], right[128];
    const char *fname = (buf && buf->filename) ? buf->filename : "[No Name]";
    const char *dirt  = (buf && buf->dirty) ? " [+]" : "";

    if (ed->status_msg[0]) {
        snprintf(left, sizeof(left), " %s", ed->status_msg);
    } else {
        snprintf(left, sizeof(left), " %s%s", fname, dirt);
    }
    snprintf(right, sizeof(right), "Ln %d, Col %d  ",
             ed->cursor_row + 1, ed->cursor_col + 1);

    int ty = y + (GUI_STATUS_HEIGHT - g_gui->char_height) / 2;
    SDL_Color fc = gui_sdl_color(g_gui->fg[cpair]);

    gui_draw_text(left, 0, ty, fc, g_gui->font);

    /* Right-aligned text */
    int rw = (int)strlen(right) * g_gui->char_width;
    gui_draw_text(right, w - rw, ty, fc, g_gui->font);
}

/* ============================================================================
 * Rendering: Gutter (line numbers + git markers + LSP markers)
 * ============================================================================ */

static void gui_render_gutter(int text_x, int text_y,
                               int visible_rows, int gutter_px __attribute__((unused)))
{
    Editor *ed = g_gui->editor;
    Buffer *buf = editor_current_buffer(ed);
    if (!buf) return;

    int cw = g_gui->char_width;
    int ch = g_gui->char_height;

    for (int r = 0; r < visible_rows; r++) {
        int line = ed->view_row + r;
        if (line >= buf->num_lines) break;

        int py = text_y + r * ch;

        /* Git gutter marker (1 character wide, at left edge) */
        if (buf->git_state.line_status && line < buf->git_state.line_count) {
            GuiRGB gc = g_gui->theme_fg;
            char marker = ' ';
            switch (buf->git_state.line_status[line]) {
                case GIT_LINE_ADDED:    gc = g_gui->fg[CPAIR_GIT_ADDED];
                                        marker = '+'; break;
                case GIT_LINE_MODIFIED: gc = g_gui->fg[CPAIR_GIT_MODIFIED];
                                        marker = '~'; break;
                case GIT_LINE_DELETED:  gc = g_gui->fg[CPAIR_GIT_DELETED];
                                        marker = '-'; break;
                default: break;
            }
            if (marker != ' ')
                gui_draw_char(marker, text_x, py, gui_sdl_color(gc), g_gui->font);
        }

        /* LSP diagnostic marker (overrides git marker if present) */
        if (buf->lsp_diagnostics.count > 0) {
            for (int d = 0; d < buf->lsp_diagnostics.count; d++) {
                if (buf->lsp_diagnostics.items[d].line == line) {
                    int sev = buf->lsp_diagnostics.items[d].severity;
                    int cp = (sev <= 1) ? CPAIR_LSP_ERROR : CPAIR_LSP_WARNING;
                    char sym = (sev <= 1) ? 'E' : 'W';
                    gui_draw_char(sym, text_x, py,
                                  gui_sdl_color(g_gui->fg[cp]), g_gui->font_bold);
                    break;
                }
            }
        }

        /* Line number (right-aligned in gutter, after the 1-char marker) */
        char lnum[16];
        snprintf(lnum, sizeof(lnum), "%4d", line + 1);
        int lx = text_x + cw;  /* skip marker column */
        gui_draw_text(lnum, lx, py,
                      gui_sdl_color(g_gui->fg[CPAIR_GUTTER]), g_gui->font);
    }
}

/* ============================================================================
 * Rendering: Text Area (buffer content with syntax highlighting)
 *
 * This is the most complex rendering function.  For each visible line:
 *   1. Draw background highlights (selection, search matches)
 *   2. Draw syntax-highlighted text
 *   3. Handle inline diff phantom lines
 * ============================================================================ */

/*
 * Helper: map a SyntaxToken to a CPAIR_* color pair index.
 */
static int syntax_token_cpair(SyntaxToken tok)
{
    switch (tok) {
        case SYN_KEYWORD: return CPAIR_SYN_KEYWORD;
        case SYN_TYPE:    return CPAIR_SYN_TYPE;
        case SYN_STRING:  return CPAIR_SYN_STRING;
        case SYN_COMMENT: return CPAIR_SYN_COMMENT;
        case SYN_PREPROC: return CPAIR_SYN_PREPROC;
        case SYN_NUMBER:  return CPAIR_SYN_NUMBER;
        default:          return 0;  /* 0 = use default fg */
    }
}

/*
 * Helper: check if a buffer position is inside the selection.
 */
static int gui_in_selection(Editor *ed, int row, int col)
{
    if (!ed->sel_active) return 0;

    /* Normalize selection: sr/sc is the earlier position */
    int sr = ed->sel_anchor_row, sc = ed->sel_anchor_col;
    int er = ed->cursor_row,     ec = ed->cursor_col;
    if (sr > er || (sr == er && sc > ec)) {
        int t = sr; sr = er; er = t;
        t = sc; sc = ec; ec = t;
    }

    if (row < sr || row > er) return 0;
    int row_start = (row == sr) ? sc : 0;
    int row_end   = (row == er) ? ec : (1 << 30);
    return col >= row_start && col < row_end;
}

/*
 * Helper: check if a column on a line is inside a search match.
 */
static int gui_in_search_match(const char *line_text, int line_len,
                                int col, const char *query, int qlen)
{
    if (qlen <= 0) return 0;
    int p_start = col - qlen + 1;
    if (p_start < 0) p_start = 0;
    for (int p = p_start; p <= col && p + qlen <= line_len; p++) {
        if (strncmp(line_text + p, query, qlen) == 0)
            return 1;
    }
    return 0;
}

static void gui_render_text_area(void)
{
    Editor *ed = g_gui->editor;
    Buffer *buf = editor_current_buffer(ed);
    if (!buf) return;

    int cw = g_gui->char_width;
    int ch = g_gui->char_height;

    /* Calculate text area bounds in pixels */
    int text_x = 0;
    if (ed->show_filetree) text_x += FILETREE_WIDTH * cw;
    int gutter_px = GUTTER_WIDTH * cw;
    text_x += gutter_px;

    /* Add blame column if active */
    int blame_px = 0;
    if (ed->show_git_blame && ed->git_blame.count > 0)
        blame_px = BLAME_WIDTH * cw;
    text_x += blame_px;

    int text_y = GUI_TAB_HEIGHT;
    int text_w = g_gui->win_width - text_x;
    if (ed->show_git_panel) text_w -= GIT_PANEL_WIDTH * cw;
    if (text_w < cw) text_w = cw;

    int text_h = g_gui->win_height - GUI_TAB_HEIGHT - GUI_STATUS_HEIGHT;
    if (ed->show_build_panel) text_h -= BUILD_PANEL_HEIGHT * ch;
    if (text_h < ch) text_h = ch;

    int visible_rows = text_h / ch;
    int visible_cols = text_w / cw;

    /* Detect syntax language for highlighting */
    SyntaxLang lang = LANG_NONE;
    if (buf->filename) lang = syntax_detect_language(buf->filename);

    /* We need to track multi-line syntax state from the top of the buffer
     * down to view_row so block comments/strings are colored correctly. */
    int ml_state = 0;
    for (int r = 0; r < ed->view_row && r < buf->num_lines; r++) {
        SyntaxToken dummy[SYNTAX_MAX_LINE];
        const char *lt = buffer_get_line(buf, r);
        int ll = buffer_line_len(buf, r);
        if (ll > SYNTAX_MAX_LINE) ll = SYNTAX_MAX_LINE;
        ml_state = syntax_highlight_line(lang, lt, ll, dummy, ml_state);
    }

    /* Search state */
    const char *query = ed->search_query;
    int qlen = (int)strlen(query);

    /* Bracket match */
    int bm_row = -1, bm_col = -1;
    editor_find_bracket_match(ed, &bm_row, &bm_col);

    /* Render the gutter */
    int gutter_x = ed->show_filetree ? FILETREE_WIDTH * cw : 0;
    gui_render_gutter(gutter_x, text_y, visible_rows, gutter_px);

    /* Render blame annotations if active */
    if (blame_px > 0) {
        int blame_x = gutter_x + gutter_px;
        for (int r = 0; r < visible_rows; r++) {
            int line = ed->view_row + r;
            if (line >= buf->num_lines || line >= ed->git_blame.count) break;

            char annot[32];
            snprintf(annot, sizeof(annot), "%.10s %.10s",
                     ed->git_blame.lines[line].author,
                     ed->git_blame.lines[line].date);

            gui_draw_text(annot, blame_x, text_y + r * ch,
                          gui_sdl_color(g_gui->fg[CPAIR_BLAME]), g_gui->font);
        }
    }

    /* ---- Main text rendering loop ---- */
    int screen_row = 0;
    int buf_line = ed->view_row;

    while (screen_row < visible_rows && buf_line < buf->num_lines) {

        /* Check for inline diff phantom lines before this buffer line */
        if (ed->show_inline_diff) {
            for (int c = 0; c < ed->inline_diff.count; c++) {
                GitDiffChunk *chunk = &ed->inline_diff.chunks[c];
                if (chunk->before_line == buf_line) {
                    for (int p = 0; p < chunk->old_count
                                    && screen_row < visible_rows; p++) {
                        int py = text_y + screen_row * ch;
                        /* Draw phantom line background (dim red) */
                        GuiRGB phantom_bg = {60, 20, 20};
                        gui_fill_rect(text_x, py, text_w, ch, phantom_bg);
                        /* Draw phantom line text */
                        const char *old_text = chunk->old_lines[p];
                        if (old_text) {
                            char prefix[8] = "- ";
                            int px = text_x;
                            gui_draw_text(prefix, px, py,
                                gui_sdl_color(g_gui->fg[CPAIR_GIT_OLD_LINE]),
                                g_gui->font);
                            px += 2 * cw;
                            gui_draw_text(old_text, px, py,
                                gui_sdl_color(g_gui->fg[CPAIR_GIT_OLD_LINE]),
                                g_gui->font);
                        }
                        screen_row++;
                    }
                }
            }
            if (screen_row >= visible_rows) break;
        }

        int py = text_y + screen_row * ch;
        const char *line_text = buffer_get_line(buf, buf_line);
        int line_len = buffer_line_len(buf, buf_line);

        /* Syntax highlight this line */
        SyntaxToken tokens[SYNTAX_MAX_LINE];
        int hl_len = line_len;
        if (hl_len > SYNTAX_MAX_LINE) hl_len = SYNTAX_MAX_LINE;
        ml_state = syntax_highlight_line(lang, line_text, hl_len,
                                          tokens, ml_state);

        /* Region highlight background */
        if (ed->region_active
            && buf_line >= ed->region_start_row
            && buf_line <= ed->region_end_row) {
            GuiRGB region_bg = {50, 20, 20};
            gui_fill_rect(text_x, py, text_w, ch, region_bg);
            /* Draw region border lines */
            GuiRGB border = g_gui->fg[CPAIR_REGION];
            if (buf_line == ed->region_start_row)
                gui_fill_rect(text_x, py, text_w, 1, border);
            if (buf_line == ed->region_end_row)
                gui_fill_rect(text_x, py + ch - 1, text_w, 1, border);
            gui_fill_rect(text_x, py, 2, ch, border);
            gui_fill_rect(text_x + text_w - 2, py, 2, ch, border);
        }

        /* Render each character with the correct background and foreground */
        int start_col = ed->view_col;
        int end_col = start_col + visible_cols;
        if (end_col > line_len) end_col = line_len;

        for (int col = start_col; col < end_col; col++) {
            int px = text_x + (col - start_col) * cw;

            /* Determine background color */
            int has_bg = 0;
            GuiRGB bg_color = g_gui->theme_bg;

            /* Selection background */
            if (gui_in_selection(ed, buf_line, col)) {
                bg_color = g_gui->bg[CPAIR_SELECTION];
                has_bg = 1;
            }

            /* Search match background */
            if (qlen > 0) {
                if (buf_line == ed->search_match_row
                    && col >= ed->search_match_col
                    && col < ed->search_match_col + qlen) {
                    bg_color = g_gui->bg[CPAIR_SEARCH_CUR];
                    has_bg = 1;
                } else if (gui_in_search_match(line_text, line_len,
                                                col, query, qlen)) {
                    bg_color = g_gui->bg[CPAIR_SEARCH_MATCH];
                    has_bg = 1;
                }
            }

            /* Bracket match background */
            if ((buf_line == ed->cursor_row && col == ed->cursor_col
                 && bm_col >= 0)
                || (buf_line == bm_row && col == bm_col)) {
                bg_color = g_gui->bg[CPAIR_BRACKET];
                has_bg = 1;
            }

            if (has_bg) {
                gui_fill_rect(px, py, cw, ch, bg_color);
            }

            /* Determine foreground color */
            GuiRGB fg_color = g_gui->theme_fg;
            TTF_Font *f = g_gui->font;

            /* Use bracket color for matched brackets */
            if ((buf_line == ed->cursor_row && col == ed->cursor_col
                 && bm_col >= 0)
                || (buf_line == bm_row && col == bm_col)) {
                fg_color = g_gui->fg[CPAIR_BRACKET];
                f = g_gui->font_bold;
            }
            /* Search match text color */
            else if (has_bg && qlen > 0) {
                /* Use the bg pair's fg for readability on colored bg */
                if (buf_line == ed->search_match_row
                    && col >= ed->search_match_col
                    && col < ed->search_match_col + qlen) {
                    fg_color = g_gui->fg[CPAIR_SEARCH_CUR];
                } else {
                    fg_color = g_gui->fg[CPAIR_SEARCH_MATCH];
                }
            }
            /* Selection text color */
            else if (gui_in_selection(ed, buf_line, col)) {
                fg_color = g_gui->fg[CPAIR_SELECTION];
            }
            /* Syntax color */
            else if (col < hl_len) {
                int cp = syntax_token_cpair(tokens[col]);
                if (cp > 0) {
                    fg_color = g_gui->fg[cp];
                    if (g_gui->bold[cp])
                        f = g_gui->font_bold;
                }
            }

            /* Draw the character */
            char c = line_text[col];
            if (ed->show_whitespace && c == ' ') {
                /* Render whitespace dot (middle dot character) */
                GuiRGB ws_color = {80, 80, 80};
                gui_draw_char('.', px + cw / 4, py,
                              gui_sdl_color(ws_color), g_gui->font);
            } else {
                gui_draw_char(c, px, py, gui_sdl_color(fg_color), f);
            }
        }

        /* LSP diagnostic underlines */
        for (int d = 0; d < buf->lsp_diagnostics.count; d++) {
            LspDiagnostic *diag = &buf->lsp_diagnostics.items[d];
            if (diag->line == buf_line) {
                int ds = diag->col;
                int de = diag->end_col;
                if (ds < start_col) ds = start_col;
                if (de > end_col) de = end_col;
                if (ds < de) {
                    int ux = text_x + (ds - start_col) * cw;
                    int uw = (de - ds) * cw;
                    int uy = py + ch - 2;
                    int cp = (diag->severity <= 1) ? CPAIR_LSP_ERROR
                                                   : CPAIR_LSP_WARNING;
                    /* Draw wavy underline as a simple line */
                    GuiRGB uc = g_gui->fg[cp];
                    SDL_SetRenderDrawColor(g_gui->renderer,
                                           uc.r, uc.g, uc.b, 255);
                    SDL_RenderDrawLine(g_gui->renderer,
                                       ux, uy, ux + uw, uy);
                    SDL_RenderDrawLine(g_gui->renderer,
                                       ux, uy + 1, ux + uw, uy + 1);
                }
            }
        }

        screen_row++;
        buf_line++;
    }

    /* ---- Cursor rendering ---- */
    {
        int crow = ed->cursor_row - ed->view_row;
        int ccol = ed->cursor_col - ed->view_col;

        /* Account for phantom lines above the cursor */
        if (ed->show_inline_diff) {
            int phantom = git_phantom_lines_in_range(
                &ed->inline_diff, ed->view_row, ed->cursor_row);
            crow += phantom;
        }

        if (crow >= 0 && crow < visible_rows
            && ccol >= 0 && ccol < visible_cols) {
            int cx = text_x + ccol * cw;
            int cy = text_y + crow * ch;

            /* Draw a thin vertical bar cursor */
            GuiRGB cursor_color = {255, 255, 255};
            gui_fill_rect(cx, cy, GUI_CURSOR_WIDTH, ch, cursor_color);
        }
    }
}

/* ============================================================================
 * Rendering: File Tree Panel (left side)
 * ============================================================================ */

static void gui_render_filetree(void)
{
    Editor *ed = g_gui->editor;
    if (!ed->show_filetree || !ed->filetree) return;

    FileTree *ft = ed->filetree;
    int cw = g_gui->char_width;
    int ch = g_gui->char_height;
    int panel_w = FILETREE_WIDTH * cw;
    int panel_y = GUI_TAB_HEIGHT;
    int panel_h = g_gui->win_height - GUI_TAB_HEIGHT - GUI_STATUS_HEIGHT;

    /* Panel background */
    GuiRGB panel_bg = {37, 37, 38};
    gui_fill_rect(0, panel_y, panel_w, panel_h, panel_bg);

    /* Border line (rightmost pixel column) */
    GuiRGB border_color = {60, 60, 60};
    gui_fill_rect(panel_w - 1, panel_y, 1, panel_h, border_color);

    /* Header */
    int header_y = panel_y;
    gui_draw_text(" Files", 0, header_y,
                  gui_sdl_color(g_gui->theme_fg), g_gui->font_bold);

    /* Entries */
    int entry_rows = (panel_h / ch) - 1;  /* minus header */

    /* Scroll adjustment */
    if (ed->filetree_cursor < ed->filetree_scroll)
        ed->filetree_scroll = ed->filetree_cursor;
    if (ed->filetree_cursor >= ed->filetree_scroll + entry_rows)
        ed->filetree_scroll = ed->filetree_cursor - entry_rows + 1;
    if (ed->filetree_scroll < 0) ed->filetree_scroll = 0;

    for (int r = 0; r < entry_rows; r++) {
        int idx = ed->filetree_scroll + r;
        if (idx >= ft->count) break;

        int ey = panel_y + (r + 1) * ch;
        FlatEntry *e = &ft->entries[idx];
        int is_cursor = (idx == ed->filetree_cursor);

        /* Cursor row highlight */
        if (is_cursor && ed->filetree_focus) {
            gui_fill_rect(0, ey, panel_w - 1, ch,
                          g_gui->bg[CPAIR_FILETREE_CURSOR]);
        }

        /* Indent */
        int indent = e->depth * 2;
        if (indent > FILETREE_WIDTH - 3) indent = FILETREE_WIDTH - 3;

        /* Icon */
        char icon = ' ';
        if (e->is_dir)
            icon = filetree_is_expanded(ft, e->path) ? 'v' : '>';

        /* Build display string */
        char display[64];
        snprintf(display, sizeof(display), "%*s%c %s",
                 indent, "", icon, e->name);
        /* Truncate to panel width */
        int max_chars = FILETREE_WIDTH - 2;
        if ((int)strlen(display) > max_chars)
            display[max_chars] = '\0';

        /* Choose color */
        GuiRGB fc;
        TTF_Font *f = g_gui->font;
        if (is_cursor && ed->filetree_focus) {
            fc = g_gui->fg[CPAIR_FILETREE_CURSOR];
        } else if (e->is_dir) {
            fc = g_gui->fg[CPAIR_FILETREE_DIR];
            f = g_gui->font_bold;
        } else {
            fc = g_gui->theme_fg;
        }

        gui_draw_text(display, 0, ey, gui_sdl_color(fc), f);
    }
}

/* ============================================================================
 * Rendering: Git Status Panel (right side)
 * ============================================================================ */

static void gui_render_git_panel(void)
{
    Editor *ed = g_gui->editor;
    if (!ed->show_git_panel || !ed->git_status) return;

    int cw = g_gui->char_width;
    int ch = g_gui->char_height;
    int panel_w = GIT_PANEL_WIDTH * cw;
    int panel_x = g_gui->win_width - panel_w;
    int panel_y = GUI_TAB_HEIGHT;
    int panel_h = g_gui->win_height - GUI_TAB_HEIGHT - GUI_STATUS_HEIGHT;

    /* Background */
    GuiRGB panel_bg = {37, 37, 38};
    gui_fill_rect(panel_x, panel_y, panel_w, panel_h, panel_bg);

    /* Left border */
    GuiRGB border_color = {60, 60, 60};
    gui_fill_rect(panel_x, panel_y, 1, panel_h, border_color);

    /* Header */
    gui_draw_text(" Git Status", panel_x + 4, panel_y,
                  gui_sdl_color(g_gui->theme_fg), g_gui->font_bold);

    /* Entries */
    GitStatusList *gs = ed->git_status;
    int entry_rows = (panel_h / ch) - 1;

    /* Scroll */
    if (ed->git_panel_cursor < ed->git_panel_scroll)
        ed->git_panel_scroll = ed->git_panel_cursor;
    if (ed->git_panel_cursor >= ed->git_panel_scroll + entry_rows)
        ed->git_panel_scroll = ed->git_panel_cursor - entry_rows + 1;
    if (ed->git_panel_scroll < 0) ed->git_panel_scroll = 0;

    for (int r = 0; r < entry_rows; r++) {
        int idx = ed->git_panel_scroll + r;
        if (idx >= gs->count) break;

        int ey = panel_y + (r + 1) * ch;
        GitStatusEntry *e = &gs->entries[idx];
        int is_cursor = (idx == ed->git_panel_cursor);

        if (is_cursor && ed->git_panel_focus) {
            gui_fill_rect(panel_x + 1, ey, panel_w - 1, ch,
                          g_gui->bg[CPAIR_GIT_PANEL_CURSOR]);
        }

        /* Status indicator and color */
        char status_ch = e->work_status;
        if (status_ch == ' ') status_ch = e->index_status;

        int is_staged = (e->index_status != ' ' && e->index_status != '?');
        GuiRGB fc;
        if (is_cursor && ed->git_panel_focus)
            fc = g_gui->fg[CPAIR_GIT_PANEL_CURSOR];
        else if (is_staged)
            fc = g_gui->fg[CPAIR_GIT_STAGED];
        else
            fc = g_gui->theme_fg;

        /* Extract basename */
        const char *name = strrchr(e->path, '/');
        name = name ? name + 1 : e->path;

        char display[64];
        snprintf(display, sizeof(display), " %c %.24s", status_ch, name);

        gui_draw_text(display, panel_x + 4, ey, gui_sdl_color(fc), g_gui->font);
    }
}

/* ============================================================================
 * Rendering: Build Panel (bottom)
 * ============================================================================ */

static void gui_render_build_panel(void)
{
    Editor *ed = g_gui->editor;
    if (!ed->show_build_panel || !ed->build_result) return;

    int cw = g_gui->char_width;
    int ch = g_gui->char_height;
    int panel_h = BUILD_PANEL_HEIGHT * ch;
    int panel_y = g_gui->win_height - GUI_STATUS_HEIGHT - panel_h;
    int panel_w = g_gui->win_width;

    /* Background */
    GuiRGB panel_bg = {30, 30, 30};
    gui_fill_rect(0, panel_y, panel_w, panel_h, panel_bg);

    /* Top border */
    GuiRGB border_color = {60, 60, 60};
    gui_fill_rect(0, panel_y, panel_w, 1, border_color);

    /* Header */
    BuildResult *br = ed->build_result;
    const char *header = br->exit_status == 0 ? " Build Succeeded"
                                               : " Build Failed";
    int hcp = br->exit_status == 0 ? CPAIR_BUILD_SUCCESS : CPAIR_BUILD_ERROR;
    gui_draw_text(header, 4, panel_y + 2,
                  gui_sdl_color(g_gui->fg[hcp]), g_gui->font_bold);

    /* Error entries */
    int entry_rows = BUILD_PANEL_HEIGHT - 1;

    if (ed->build_panel_cursor < ed->build_panel_scroll)
        ed->build_panel_scroll = ed->build_panel_cursor;
    if (ed->build_panel_cursor >= ed->build_panel_scroll + entry_rows)
        ed->build_panel_scroll = ed->build_panel_cursor - entry_rows + 1;
    if (ed->build_panel_scroll < 0) ed->build_panel_scroll = 0;

    for (int r = 0; r < entry_rows; r++) {
        int idx = ed->build_panel_scroll + r;
        if (idx >= br->error_count) break;

        int ey = panel_y + (r + 1) * ch;
        BuildError *e = &br->errors[idx];
        int is_cursor = (idx == ed->build_panel_cursor);

        if (is_cursor && ed->build_panel_focus) {
            gui_fill_rect(0, ey, panel_w, ch,
                          g_gui->bg[CPAIR_BUILD_CURSOR]);
        }

        /* Build display string */
        const char *basename = strrchr(e->filepath, '/');
        basename = basename ? basename + 1 : e->filepath;

        char display[256];
        snprintf(display, sizeof(display), " %s:%d: %s",
                 basename, e->line, e->message);
        /* Truncate */
        int max_chars = panel_w / cw - 1;
        if (max_chars > 0 && (int)strlen(display) > max_chars)
            display[max_chars] = '\0';

        GuiRGB fc;
        if (is_cursor && ed->build_panel_focus)
            fc = g_gui->fg[CPAIR_BUILD_CURSOR];
        else if (e->is_warning)
            fc = g_gui->fg[CPAIR_BUILD_WARNING];
        else
            fc = g_gui->fg[CPAIR_BUILD_ERROR];

        gui_draw_text(display, 4, ey, gui_sdl_color(fc), g_gui->font);
    }

    (void)cw;  /* suppress unused warning if panel is narrow */
}

/* ============================================================================
 * Main Render Function
 * ============================================================================ */

static void gui_render(void)
{
    /* Clear the entire window with the theme background color */
    SDL_SetRenderDrawColor(g_gui->renderer,
        g_gui->theme_bg.r, g_gui->theme_bg.g, g_gui->theme_bg.b, 255);
    SDL_RenderClear(g_gui->renderer);

    /* Recalculate term_rows/cols (panels may have changed) */
    gui_update_term_size();

    /* Render each component */
    gui_render_tab_bar();
    gui_render_filetree();
    gui_render_text_area();
    gui_render_git_panel();
    gui_render_build_panel();
    gui_render_status_bar();
}

/* ============================================================================
 * Modal Prompt — replacement for display_prompt()
 *
 * Runs a nested SDL event loop that renders the editor with a text input
 * field overlaid on the status bar.  Returns a heap-allocated string
 * (like strdup) when the user presses Enter, or NULL on Escape.
 * ============================================================================ */

static char *gui_prompt_impl(struct Editor *ed, const char *prompt)
{
    char input[256] = {0};
    int  len = 0;

    /* Enable text input for this dialog */
    SDL_StartTextInput();

    for (;;) {
        /* Render the editor behind the prompt */
        gui_render();

        /* Draw the prompt overlay on the status bar area */
        int y = g_gui->win_height - GUI_STATUS_HEIGHT;
        int w = g_gui->win_width;
        GuiRGB prompt_bg = g_gui->bg[CPAIR_STATUS];
        gui_fill_rect(0, y, w, GUI_STATUS_HEIGHT, prompt_bg);

        /* Prompt text + user input */
        char display[512];
        snprintf(display, sizeof(display), "%s%s", prompt, input);
        int ty = y + (GUI_STATUS_HEIGHT - g_gui->char_height) / 2;
        gui_draw_text(display, 4, ty,
                      gui_sdl_color(g_gui->fg[CPAIR_STATUS]),
                      g_gui->font_bold);

        /* Draw cursor in the input field */
        int cx = 4 + ((int)strlen(prompt) + len) * g_gui->char_width;
        GuiRGB cursor_c = {255, 255, 255};
        gui_fill_rect(cx, ty, GUI_CURSOR_WIDTH, g_gui->char_height, cursor_c);

        SDL_RenderPresent(g_gui->renderer);

        /* Process events */
        SDL_Event event;
        if (!SDL_WaitEvent(&event)) continue;

        do {
            if (event.type == SDL_QUIT) {
                ed->should_quit = 1;
                SDL_StopTextInput();
                return NULL;
            }

            if (event.type == SDL_KEYDOWN) {
                SDL_Keycode key = event.key.keysym.sym;

                if (key == SDLK_RETURN || key == SDLK_KP_ENTER) {
                    SDL_StopTextInput();
                    return strdup(input);
                }
                if (key == SDLK_ESCAPE) {
                    SDL_StopTextInput();
                    return NULL;
                }
                if (key == SDLK_BACKSPACE && len > 0) {
                    input[--len] = '\0';
                }
            }

            if (event.type == SDL_TEXTINPUT) {
                for (int i = 0; event.text.text[i] && len < 255; i++) {
                    unsigned char c = (unsigned char)event.text.text[i];
                    if (c >= 0x20 && c <= 0x7e) {
                        input[len++] = (char)c;
                        input[len] = '\0';
                    }
                }
            }

            if (event.type == SDL_WINDOWEVENT
                && event.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
                g_gui->win_width  = event.window.data1;
                g_gui->win_height = event.window.data2;
                gui_update_term_size();
            }
        } while (SDL_PollEvent(&event));
    }
}

/* ============================================================================
 * Modal Finder Popup — replacement for display_finder_popup()
 *
 * Centered popup with a search input field and filtered file list.
 * Runs its own SDL event loop.  Returns a heap-allocated path or NULL.
 * ============================================================================ */

static char *gui_finder_impl(struct Editor *ed,
                              FinderFile *files, int num_files)
{
    (void)ed;

    char query[FINDER_QUERY_MAX] = {0};
    int  query_len = 0;

    FinderResult *results = malloc(FINDER_MAX_RESULTS * sizeof(FinderResult));
    if (!results) return NULL;

    int num_results = finder_filter(files, num_files, query,
                                     results, FINDER_MAX_RESULTS);
    int cursor = 0;
    int scroll = 0;

    SDL_StartTextInput();

    int cw = g_gui->char_width;
    int ch = g_gui->char_height;

    for (;;) {
        /* Calculate popup dimensions */
        int popup_w = g_gui->win_width - 100;
        if (popup_w > 80 * cw) popup_w = 80 * cw;
        if (popup_w < 20 * cw) popup_w = 20 * cw;

        int popup_h = g_gui->win_height - 100;
        if (popup_h > 20 * ch) popup_h = 20 * ch;
        if (popup_h < 5 * ch)  popup_h = 5 * ch;

        int popup_x = (g_gui->win_width - popup_w) / 2;
        int popup_y = (g_gui->win_height - popup_h) / 2;

        int result_rows = popup_h / ch - 2;  /* minus input row and padding */
        if (result_rows < 1) result_rows = 1;

        /* Scroll to keep cursor visible */
        if (cursor < scroll) scroll = cursor;
        if (cursor >= scroll + result_rows)
            scroll = cursor - result_rows + 1;
        if (scroll < 0) scroll = 0;

        /* Render the editor behind the popup (dimmed) */
        gui_render();

        /* Dim overlay */
        SDL_SetRenderDrawBlendMode(g_gui->renderer, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(g_gui->renderer, 0, 0, 0, 128);
        SDL_Rect dim = {0, 0, g_gui->win_width, g_gui->win_height};
        SDL_RenderFillRect(g_gui->renderer, &dim);
        SDL_SetRenderDrawBlendMode(g_gui->renderer, SDL_BLENDMODE_NONE);

        /* Popup background */
        GuiRGB popup_bg = {45, 45, 48};
        gui_fill_rect(popup_x, popup_y, popup_w, popup_h, popup_bg);

        /* Popup border */
        GuiRGB border = g_gui->fg[CPAIR_FINDER_BORDER];
        gui_fill_rect(popup_x, popup_y, popup_w, 1, border);
        gui_fill_rect(popup_x, popup_y + popup_h - 1, popup_w, 1, border);
        gui_fill_rect(popup_x, popup_y, 1, popup_h, border);
        gui_fill_rect(popup_x + popup_w - 1, popup_y, 1, popup_h, border);

        /* Title */
        gui_draw_text(" Find File ", popup_x + 8, popup_y - ch / 4,
                      gui_sdl_color(border), g_gui->font_bold);

        /* Input row */
        int input_y = popup_y + 4;
        char input_display[256];
        snprintf(input_display, sizeof(input_display), "> %s", query);
        gui_draw_text(input_display, popup_x + 8, input_y,
                      gui_sdl_color(g_gui->theme_fg), g_gui->font_bold);

        /* Input cursor */
        int icx = popup_x + 8 + (2 + query_len) * cw;
        GuiRGB cursor_c = {255, 255, 255};
        gui_fill_rect(icx, input_y, GUI_CURSOR_WIDTH, ch, cursor_c);

        /* Separator line below input */
        int sep_y = input_y + ch + 2;
        GuiRGB sep_color = {60, 60, 60};
        gui_fill_rect(popup_x + 4, sep_y, popup_w - 8, 1, sep_color);

        /* Result rows */
        for (int r = 0; r < result_rows; r++) {
            int idx = scroll + r;
            int ry = sep_y + 4 + r * ch;

            if (idx >= num_results) break;

            int is_cur = (idx == cursor);
            const char *disp = files[results[idx].index].display;

            if (is_cur) {
                gui_fill_rect(popup_x + 2, ry, popup_w - 4, ch,
                              g_gui->bg[CPAIR_FINDER_CURSOR]);
                gui_draw_text(disp, popup_x + 12, ry,
                    gui_sdl_color(g_gui->fg[CPAIR_FINDER_CURSOR]),
                    g_gui->font_bold);
            } else {
                gui_draw_text(disp, popup_x + 12, ry,
                    gui_sdl_color(g_gui->theme_fg), g_gui->font);
            }
        }

        /* Result count in bottom-right */
        char count_str[32];
        snprintf(count_str, sizeof(count_str), " %d/%d ",
                 num_results, num_files);
        int count_w = (int)strlen(count_str) * cw;
        gui_draw_text(count_str,
                      popup_x + popup_w - count_w - 8,
                      popup_y + popup_h - ch - 2,
                      gui_sdl_color(border), g_gui->font);

        SDL_RenderPresent(g_gui->renderer);

        /* Handle events */
        SDL_Event event;
        if (!SDL_WaitEvent(&event)) continue;

        do {
            if (event.type == SDL_QUIT) {
                ed->should_quit = 1;
                SDL_StopTextInput();
                free(results);
                return NULL;
            }

            if (event.type == SDL_KEYDOWN) {
                SDL_Keycode key = event.key.keysym.sym;

                if (key == SDLK_ESCAPE) {
                    SDL_StopTextInput();
                    free(results);
                    return NULL;
                }
                if (key == SDLK_RETURN || key == SDLK_KP_ENTER) {
                    char *selected = NULL;
                    if (num_results > 0 && cursor >= 0
                        && cursor < num_results) {
                        selected = strdup(
                            files[results[cursor].index].path);
                    }
                    SDL_StopTextInput();
                    free(results);
                    return selected;
                }
                if (key == SDLK_UP) {
                    if (cursor > 0) cursor--;
                }
                if (key == SDLK_DOWN) {
                    if (cursor < num_results - 1) cursor++;
                }
                if (key == SDLK_BACKSPACE && query_len > 0) {
                    query[--query_len] = '\0';
                    cursor = 0;
                    scroll = 0;
                    num_results = finder_filter(files, num_files, query,
                                                results, FINDER_MAX_RESULTS);
                }
            }

            if (event.type == SDL_TEXTINPUT) {
                for (int i = 0; event.text.text[i]
                            && query_len < FINDER_QUERY_MAX - 1; i++) {
                    unsigned char c = (unsigned char)event.text.text[i];
                    if (c >= 0x20 && c <= 0x7e) {
                        query[query_len++] = (char)c;
                        query[query_len] = '\0';
                    }
                }
                cursor = 0;
                scroll = 0;
                num_results = finder_filter(files, num_files, query,
                                            results, FINDER_MAX_RESULTS);
            }

            if (event.type == SDL_WINDOWEVENT
                && event.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
                g_gui->win_width  = event.window.data1;
                g_gui->win_height = event.window.data2;
            }
        } while (SDL_PollEvent(&event));
    }
}

/* ============================================================================
 * Theme Override — replacement for display_apply_theme()
 *
 * Called from input.c when the user presses F6 to cycle themes.
 * Rebuilds the cached RGB color arrays from the new active theme.
 * ============================================================================ */

static void gui_apply_theme_impl(const void *theme_ptr)
{
    (void)theme_ptr;
    gui_cache_theme_colors();
}

/*
 * gui_render_and_present — render override for display_render().
 *
 * editor.c calls display_render() during blocking operations (LSP start,
 * build, etc.) to show status messages.  In the GUI, we redirect this to
 * render a full frame and present it.
 */
static void gui_render_impl(struct Editor *ed)
{
    (void)ed;
    if (!g_gui || !g_gui->renderer) return;
    gui_render();
    SDL_RenderPresent(g_gui->renderer);
}

/* ============================================================================
 * SDL Key → ncurses Key Mapping
 *
 * Translates SDL keyboard events into the ncurses KEY_* integer constants
 * that input_process_key_with() expects.  This lets us reuse ALL the
 * key binding logic in input.c without duplicating it.
 *
 * We need to include ncurses.h to get the KEY_* values.  This does NOT
 * initialize ncurses — the constants are just integers (#define'd).
 * ============================================================================ */

#include <ncurses.h>

/* Helper: compute a Ctrl+letter control code (same as ncurses CTRL macro) */
#define GUI_CTRL(x) ((x) & 0x1F)

/*
 * sdl_to_ncurses_key — map an SDL keysym to an ncurses key code.
 *
 * Returns the ncurses key code, or -1 if this event should be handled
 * by SDL_TEXTINPUT instead (printable characters).
 */
static int sdl_to_ncurses_key(SDL_Keysym ks)
{
    SDL_Keycode key = ks.sym;
    int shift = (ks.mod & KMOD_SHIFT) != 0;
    int ctrl  = (ks.mod & KMOD_CTRL)  != 0;

    /* ---- Arrow keys ---- */
    if (key == SDLK_UP    && shift) return KEY_SR;
    if (key == SDLK_DOWN  && shift) return KEY_SF;
    if (key == SDLK_LEFT  && shift) return KEY_SLEFT;
    if (key == SDLK_RIGHT && shift) return KEY_SRIGHT;

    if (key == SDLK_UP)    return KEY_UP;
    if (key == SDLK_DOWN)  return KEY_DOWN;
    if (key == SDLK_LEFT)  return KEY_LEFT;
    if (key == SDLK_RIGHT) return KEY_RIGHT;

    /* ---- Home / End / Page ---- */
    if (key == SDLK_HOME && shift) return KEY_SHOME;
    if (key == SDLK_END  && shift) return KEY_SEND;
    if (key == SDLK_HOME && ctrl)  return 554;   /* Ctrl+Home */
    if (key == SDLK_END  && ctrl)  return 549;   /* Ctrl+End  */
    if (key == SDLK_HOME) return KEY_HOME;
    if (key == SDLK_END)  return KEY_END;

    if (key == SDLK_PAGEUP)   return KEY_PPAGE;
    if (key == SDLK_PAGEDOWN) return KEY_NPAGE;

    /* ---- Editing keys ---- */
    if (key == SDLK_BACKSPACE) return KEY_BACKSPACE;
    if (key == SDLK_DELETE)    return KEY_DC;
    if (key == SDLK_RETURN || key == SDLK_KP_ENTER) return '\n';
    if (key == SDLK_TAB)       return '\t';
    if (key == SDLK_ESCAPE)    return 27;

    /* ---- Function keys ---- */
    if (key >= SDLK_F1 && key <= SDLK_F12) {
        int fn = key - SDLK_F1 + 1;
        if (shift) fn += 12;   /* Shift+Fn = KEY_F(n+12) in ncurses */
        return KEY_F(fn);
    }

    /* ---- Ctrl+letter ---- */
    if (ctrl && key >= SDLK_a && key <= SDLK_z) {
        return GUI_CTRL(key);  /* Ctrl+A=1, Ctrl+B=2, ... Ctrl+Z=26 */
    }

    /* ---- Ctrl+Space (LSP completion trigger) ---- */
    if (ctrl && key == SDLK_SPACE) return 0;

    /* ---- Buffer switching: Ctrl+] and Ctrl+\ ---- */
    if (ctrl && key == SDLK_RIGHTBRACKET) return 29;  /* Ctrl+] */
    if (ctrl && key == SDLK_BACKSLASH)    return 28;  /* Ctrl+\ */

    /* Unhandled — will be processed as SDL_TEXTINPUT */
    return -1;
}

/* ============================================================================
 * Mouse Handling
 * ============================================================================ */

static void gui_handle_mouse(SDL_Event *event)
{
    Editor *ed = g_gui->editor;
    int cw = g_gui->char_width;
    int ch = g_gui->char_height;

    if (event->type == SDL_MOUSEWHEEL) {
        /* Scroll the text area */
        int scroll_amount = -event->wheel.y * 3;  /* 3 lines per notch */
        ed->view_row += scroll_amount;
        Buffer *buf = editor_current_buffer(ed);
        if (buf) {
            if (ed->view_row < 0) ed->view_row = 0;
            int max_view = buf->num_lines - 1;
            if (ed->view_row > max_view) ed->view_row = max_view;
        }
        return;
    }

    if (event->type == SDL_MOUSEBUTTONDOWN
        && event->button.button == SDL_BUTTON_LEFT) {

        int mx = event->button.x;
        int my = event->button.y;

        /* ---- Click on tab bar ---- */
        if (my < GUI_TAB_HEIGHT) {
            int x = 0;
            for (int i = 0; i < ed->num_buffers; i++) {
                Buffer *buf = ed->buffers[i];
                const char *name = buf->filename
                    ? strrchr(buf->filename, '/') : NULL;
                name = name ? name + 1
                    : (buf->filename ? buf->filename : "[No Name]");
                const char *dirt = buf->dirty ? " [+]" : "";
                char label[64];
                snprintf(label, sizeof(label), " %.20s%s ", name, dirt);
                int lw = (int)strlen(label) * cw;

                if (mx >= x && mx < x + lw) {
                    /* Switch to this buffer */
                    if (i != ed->current_buffer) {
                        /* Save current cursor position */
                        Buffer *cur = editor_current_buffer(ed);
                        if (cur) {
                            cur->cursor_row  = ed->cursor_row;
                            cur->cursor_col  = ed->cursor_col;
                            cur->desired_col = ed->desired_col;
                            cur->view_row    = ed->view_row;
                            cur->view_col    = ed->view_col;
                        }
                        ed->current_buffer = i;
                        Buffer *nb = ed->buffers[i];
                        ed->cursor_row  = nb->cursor_row;
                        ed->cursor_col  = nb->cursor_col;
                        ed->desired_col = nb->desired_col;
                        ed->view_row    = nb->view_row;
                        ed->view_col    = nb->view_col;
                        editor_scroll(ed);
                    }
                    return;
                }
                x += lw + 1;  /* +1 for separator */
            }
            return;
        }

        /* ---- Click on file tree ---- */
        if (ed->show_filetree && mx < FILETREE_WIDTH * cw) {
            int row = (my - GUI_TAB_HEIGHT) / ch - 1;  /* -1 for header */
            if (row >= 0) {
                int idx = ed->filetree_scroll + row;
                if (idx < ed->filetree->count) {
                    ed->filetree_cursor = idx;
                    ed->filetree_focus = 1;
                    /* Double-click could open — for now single click selects,
                     * user presses Enter to open (handled by input.c) */
                }
            }
            return;
        }

        /* ---- Click on git panel ---- */
        if (ed->show_git_panel && ed->git_status
            && mx > g_gui->win_width - GIT_PANEL_WIDTH * cw) {
            int row = (my - GUI_TAB_HEIGHT) / ch - 1;
            if (row >= 0) {
                int idx = ed->git_panel_scroll + row;
                if (idx < ed->git_status->count) {
                    ed->git_panel_cursor = idx;
                    ed->git_panel_focus = 1;
                }
            }
            return;
        }

        /* ---- Click on build panel ---- */
        if (ed->show_build_panel && ed->build_result) {
            int build_y = g_gui->win_height - GUI_STATUS_HEIGHT
                        - BUILD_PANEL_HEIGHT * ch;
            if (my > build_y && my < g_gui->win_height - GUI_STATUS_HEIGHT) {
                int row = (my - build_y) / ch - 1;
                if (row >= 0) {
                    int idx = ed->build_panel_scroll + row;
                    if (idx < ed->build_result->error_count) {
                        ed->build_panel_cursor = idx;
                        ed->build_panel_focus = 1;
                    }
                }
                return;
            }
        }

        /* ---- Click in text area — position cursor ---- */
        {
            /* Calculate text area origin */
            int text_x = 0;
            if (ed->show_filetree) text_x += FILETREE_WIDTH * cw;
            text_x += GUTTER_WIDTH * cw;
            if (ed->show_git_blame && ed->git_blame.count > 0)
                text_x += BLAME_WIDTH * cw;

            int text_y = GUI_TAB_HEIGHT;

            if (mx >= text_x && my >= text_y) {
                int row = (my - text_y) / ch;
                int col = (mx - text_x) / cw;

                int new_row = ed->view_row + row;
                int new_col = ed->view_col + col;

                Buffer *buf = editor_current_buffer(ed);
                if (buf) {
                    /* Clamp to buffer bounds */
                    if (new_row >= buf->num_lines)
                        new_row = buf->num_lines - 1;
                    if (new_row < 0) new_row = 0;

                    int ll = buffer_line_len(buf, new_row);
                    if (new_col > ll) new_col = ll;
                    if (new_col < 0) new_col = 0;

                    /* Clear selection on plain click */
                    editor_selection_clear(ed);

                    ed->cursor_row  = new_row;
                    ed->cursor_col  = new_col;
                    ed->desired_col = new_col;
                    editor_scroll(ed);

                    /* Return focus to editor */
                    ed->filetree_focus = 0;
                    ed->git_panel_focus = 0;
                    ed->build_panel_focus = 0;
                }
            }
        }
    }
}

/* ============================================================================
 * Display Update Size Override
 *
 * Called when input.c handles KEY_RESIZE.  In the GUI, we handle window
 * resizing via SDL_WINDOWEVENT and call gui_update_term_size() directly,
 * so this is a no-op.  But it must exist to avoid calling the ncurses
 * getmaxyx(stdscr, ...) which would crash.
 * ============================================================================ */

static void gui_update_size_impl(struct Editor *ed)
{
    (void)ed;
    gui_update_term_size();
}

/* ============================================================================
 * gui_main — Entry Point
 * ============================================================================ */

int gui_main(int argc, char *argv[])
{
    /*
     * Ignore SIGINT so Ctrl+C does not kill the process.
     *
     * In the TUI, ncurses' raw() prevents Ctrl+C from generating SIGINT.
     * In the GUI, ncurses is never initialized, so without this the
     * default SIGINT handler would terminate the process on Ctrl+C.
     */
    signal(SIGINT, SIG_IGN);

    /* ---- Initialize SDL2 ---- */
    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        fprintf(stderr, "texty: SDL_Init failed: %s\n", SDL_GetError());
        return EXIT_FAILURE;
    }

    if (TTF_Init() < 0) {
        fprintf(stderr, "texty: TTF_Init failed: %s\n", TTF_GetError());
        SDL_Quit();
        return EXIT_FAILURE;
    }

    /* ---- Create GuiState ---- */
    GuiState gui;
    memset(&gui, 0, sizeof(gui));
    g_gui = &gui;

    /* ---- Create window ---- */
    gui.window = SDL_CreateWindow(
        "texty",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        GUI_DEFAULT_WIDTH, GUI_DEFAULT_HEIGHT,
        SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI
    );
    if (!gui.window) {
        fprintf(stderr, "texty: SDL_CreateWindow failed: %s\n",
                SDL_GetError());
        TTF_Quit();
        SDL_Quit();
        return EXIT_FAILURE;
    }

    /* ---- Create renderer ---- */
    gui.renderer = SDL_CreateRenderer(gui.window, -1,
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!gui.renderer) {
        /* Fall back to software renderer */
        gui.renderer = SDL_CreateRenderer(gui.window, -1,
            SDL_RENDERER_SOFTWARE);
    }
    if (!gui.renderer) {
        fprintf(stderr, "texty: SDL_CreateRenderer failed: %s\n",
                SDL_GetError());
        SDL_DestroyWindow(gui.window);
        TTF_Quit();
        SDL_Quit();
        return EXIT_FAILURE;
    }

    /* ---- Load font ---- */
    if (gui_load_fonts(GUI_FONT_SIZE) != 0) {
        fprintf(stderr, "texty: could not find a monospace font.\n");
        fprintf(stderr, "  Tried:");
        for (int i = 0; font_search_paths[i]; i++)
            fprintf(stderr, " %s", font_search_paths[i]);
        fprintf(stderr, "\n");
        SDL_DestroyRenderer(gui.renderer);
        SDL_DestroyWindow(gui.window);
        TTF_Quit();
        SDL_Quit();
        return EXIT_FAILURE;
    }

    /* Get actual window size (may differ from request on HiDPI) */
    SDL_GetWindowSize(gui.window, &gui.win_width, &gui.win_height);

    /* ---- Initialize the editor ---- */
    Editor ed;
    editor_init(&ed);
    gui.editor = &ed;

    /* ---- Install display overrides ----
     *
     * These redirect display_prompt(), display_finder_popup(), and
     * display_apply_theme() to our GUI implementations so editor.c's
     * calls work without ncurses.
     */
    display_set_prompt_handler(gui_prompt_impl);
    display_set_finder_handler(gui_finder_impl);
    display_set_theme_handler(gui_apply_theme_impl);
    display_set_size_handler(gui_update_size_impl);
    display_set_render_handler(gui_render_impl);

    /* ---- Cache theme colors ---- */
    gui_cache_theme_colors();

    /* ---- Set initial terminal size ---- */
    gui_update_term_size();

    /* ---- Open file or create empty buffer ---- */
    {
        const char *file_to_open = NULL;
        for (int i = 1; i < argc; i++) {
            if (strcmp(argv[i], "-G") != 0) {
                file_to_open = argv[i];
                break;
            }
        }

        if (file_to_open) {
            if (editor_open_file(&ed, file_to_open) != 0) {
                fprintf(stderr, "texty: cannot open '%s'\n", file_to_open);
                /* Continue with empty buffer */
                editor_new_buffer(&ed);
            }
        } else {
            editor_new_buffer(&ed);
        }
    }

    /* ---- Start LSP ---- */
    editor_lsp_start(&ed);

    /* ---- Enable text input for regular typing ---- */
    SDL_StartTextInput();

    /* ---- Main Event Loop ---- */

    /*
     * suppress_text_input — when a Ctrl+key combo is handled via
     * SDL_KEYDOWN, we set this flag so the next SDL_TEXTINPUT event
     * is ignored.  On macOS, Ctrl+V can cause the system text input
     * layer to fire SDL_TEXTINPUT with system clipboard contents,
     * which we do not want (we handle paste ourselves).
     */
    int suppress_text_input = 0;

    while (!ed.should_quit) {
        /* Render the current state */
        gui_render();
        SDL_RenderPresent(gui.renderer);

        /* Wait for events (with timeout for LSP polling) */
        SDL_Event event;
        int timeout = ed.lsp_server ? 100 : 500;

        if (SDL_WaitEventTimeout(&event, timeout)) {
            /* Process all pending events */
            do {
                switch (event.type) {

                case SDL_QUIT:
                    ed.should_quit = 1;
                    break;

                case SDL_WINDOWEVENT:
                    if (event.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
                        gui.win_width  = event.window.data1;
                        gui.win_height = event.window.data2;
                        gui_update_term_size();
                        editor_scroll(&ed);
                    }
                    break;

                case SDL_KEYDOWN: {
                    int mapped = sdl_to_ncurses_key(event.key.keysym);
                    if (mapped >= 0) {
                        /*
                         * System clipboard integration.
                         *
                         * The TUI uses an internal clipboard (ed.clipboard)
                         * because terminals have no direct clipboard access.
                         * In the GUI, we bridge the system clipboard:
                         *
                         *   Ctrl+V: load system clipboard → ed.clipboard
                         *           BEFORE input.c calls editor_paste.
                         *
                         *   Ctrl+C / Ctrl+X: let input.c call editor_copy /
                         *           editor_cut, THEN push ed.clipboard to
                         *           the system clipboard.
                         */
                        if (mapped == GUI_CTRL('v')) {
                            char *sys = SDL_GetClipboardText();
                            if (sys && sys[0]) {
                                free(ed.clipboard);
                                ed.clipboard = strdup(sys);
                            }
                            SDL_free(sys);
                        }

                        input_process_key_with(&ed, mapped);

                        if (mapped == GUI_CTRL('c')
                            || mapped == GUI_CTRL('x')) {
                            if (ed.clipboard)
                                SDL_SetClipboardText(ed.clipboard);
                        }

                        /*
                         * Suppress the next SDL_TEXTINPUT — on macOS the
                         * text input system can generate spurious text
                         * events for Ctrl+key combos (especially Ctrl+V).
                         */
                        suppress_text_input = 1;
                    }
                    break;
                }

                case SDL_TEXTINPUT: {
                    /*
                     * If we just handled a Ctrl+key combo, skip this
                     * event — it may contain system clipboard contents
                     * or control characters we don't want inserted.
                     */
                    if (suppress_text_input) {
                        suppress_text_input = 0;
                        break;
                    }

                    /*
                     * Normal printable character input.  We pass each
                     * character to input_process_key_with so auto-close
                     * brackets and other character-level logic in
                     * input.c works correctly.
                     */
                    for (int i = 0; event.text.text[i]; i++) {
                        unsigned char c = (unsigned char)event.text.text[i];
                        if (c >= 0x20 && c <= 0x7e) {
                            input_process_key_with(&ed, c);
                        }
                    }
                    break;
                }

                case SDL_MOUSEBUTTONDOWN:
                case SDL_MOUSEWHEEL:
                    gui_handle_mouse(&event);
                    break;

                default:
                    break;
                }
            } while (SDL_PollEvent(&event));
        }

        /* Poll for LSP messages (non-blocking) */
        editor_lsp_poll(&ed);
    }

    /* ---- Cleanup ---- */
    SDL_StopTextInput();
    editor_cleanup(&ed);

    if (gui.font_bold && gui.font_bold != gui.font)
        TTF_CloseFont(gui.font_bold);
    TTF_CloseFont(gui.font);

    SDL_DestroyRenderer(gui.renderer);
    SDL_DestroyWindow(gui.window);

    TTF_Quit();
    SDL_Quit();

    g_gui = NULL;
    return EXIT_SUCCESS;
}

#endif /* HAS_GUI */
