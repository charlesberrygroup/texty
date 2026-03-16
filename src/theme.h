/*
 * theme.h — Color Theme Support (pure logic, no ncurses)
 * =============================================================================
 * This module defines color themes and handles parsing theme files.
 *
 * A theme is a mapping of each CPAIR_* color pair ID to a foreground color,
 * background color, and optional bold attribute.  Themes can be built-in
 * (compiled into the binary) or loaded from ~/.config/texty/themes/ .
 *
 * The display module (display.c) applies themes by calling init_pair() for
 * each color pair — that function lives in display.c, not here, to keep
 * this module free of ncurses dependencies and fully testable.
 * =============================================================================
 */

#ifndef THEME_H
#define THEME_H

/* ---- Constants ------------------------------------------------------------ */

/** Total number of color pair slots (1-indexed, matching CPAIR_* constants). */
#define THEME_NUM_PAIRS   36

/** Maximum theme name length. */
#define THEME_NAME_MAX    64

/** Maximum number of themes (built-in + loaded from disk). */
#define THEME_MAX_THEMES  16

/* ---- Data Types ----------------------------------------------------------- */

/*
 * ThemeColorDef — one color pair definition.
 *
 * fg/bg are ncurses COLOR_* integer values (0-7) or -1 for terminal default.
 * bold is 1 if A_BOLD should be applied when using this color pair.
 */
typedef struct {
    short fg;      /* foreground color (-1 = terminal default) */
    short bg;      /* background color (-1 = terminal default) */
    int   bold;    /* 1 = apply A_BOLD attribute               */
} ThemeColorDef;

/*
 * Theme — a complete color scheme.
 *
 * pairs[] is 1-indexed to match the CPAIR_* constants in display.h.
 * pairs[0] is unused.  pairs[CPAIR_GUTTER] defines the gutter colors, etc.
 */
typedef struct {
    char          name[THEME_NAME_MAX];
    ThemeColorDef pairs[THEME_NUM_PAIRS + 1];  /* 1-indexed */

    /*
     * default_fg / default_bg — the terminal's base foreground and background.
     *
     * Applied with assume_default_colors(fg, bg) which tells ncurses what
     * the terminal's default colors are.  This affects all color pairs that
     * use -1 (terminal default) for their fg or bg.
     *
     * Set to -1 to keep the terminal's own default (e.g. whatever the user
     * configured in iTerm2/Terminal.app).
     */
    short default_fg;   /* -1 = keep terminal default */
    short default_bg;   /* -1 = keep terminal default */
} Theme;

/*
 * ThemeManager — holds all available themes and the active selection.
 */
typedef struct {
    Theme  themes[THEME_MAX_THEMES];
    int    count;          /* number of loaded themes          */
    int    active_index;   /* index into themes[] of the active theme */
} ThemeManager;

/* ---- Lifecycle ------------------------------------------------------------ */

/**
 * theme_init — initialize the theme manager with built-in themes.
 *
 * Populates the manager with 4 built-in themes: Default Dark, Default Light,
 * Solarized Dark, and Monokai.  Sets the active theme to Default Dark (index 0).
 */
void theme_init(ThemeManager *tm);

/* ---- Theme selection ------------------------------------------------------ */

/**
 * theme_cycle — advance to the next theme.
 *
 * Wraps around to the first theme after the last.
 * Does NOT call ncurses — the caller must call display_apply_theme() afterward.
 * Returns the name of the newly active theme.
 */
const char *theme_cycle(ThemeManager *tm);

/**
 * theme_set_by_name — activate the theme with the given name.
 *
 * Case-sensitive match.  Returns 0 on success, -1 if not found.
 */
int theme_set_by_name(ThemeManager *tm, const char *name);

/**
 * theme_active — return a pointer to the currently active theme.
 */
const Theme *theme_active(const ThemeManager *tm);

/* ---- Parsing (pure logic, testable without ncurses) ----------------------- */

/**
 * theme_color_from_name — map a color name string to an ncurses constant.
 *
 * Supported names: "black", "red", "green", "yellow", "blue", "magenta",
 * "cyan", "white", "-1", "default".
 *
 * Returns the color integer (0-7 or -1), or -2 if unrecognized.
 */
short theme_color_from_name(const char *name);

/**
 * theme_parse — parse a .theme file's text content into a Theme struct.
 *
 * The format is key = fg [bg] per line, with # comments.
 * Unrecognized keys are silently skipped.  Missing keys keep their
 * default values (from Default Dark).
 *
 * Returns 0 on success, -1 on error.
 */
int theme_parse(Theme *out, const char *text);

/* ---- Disk loading --------------------------------------------------------- */

/**
 * theme_load_from_disk — load .theme files from ~/.config/texty/themes/.
 *
 * Each file is parsed and added to the theme manager (up to THEME_MAX_THEMES).
 */
void theme_load_from_disk(ThemeManager *tm);

/* ---- Config integration --------------------------------------------------- */

/**
 * theme_parse_config — extract the "theme" key from a texty.json string.
 *
 * Writes the theme name into `out_name`.  If the key is not found,
 * `out_name` is set to an empty string.
 */
void theme_parse_config(char *out_name, int out_size, const char *json_text);

#endif /* THEME_H */
