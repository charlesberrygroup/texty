/*
 * theme.c — Color Theme Support Implementation
 * =============================================================================
 * Implements the functions declared in theme.h.
 *
 * Built-in themes are defined as static arrays.  Theme files are parsed
 * line-by-line from a simple "key = fg [bg]" format.
 *
 * This module has NO ncurses dependency — all ncurses calls (init_pair)
 * live in display.c via display_apply_theme().  This keeps theme.c fully
 * testable in the unit test suite.
 * =============================================================================
 */

#include "theme.h"
#include "display.h"   /* for CPAIR_* constants */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>    /* for opendir, readdir — loading themes from disk */

/* ============================================================================
 * Color name lookup
 * ============================================================================ */

/*
 * ncurses COLOR_* constants (values 0-7).
 * We define them here as plain integers so theme.c doesn't need <ncurses.h>.
 * These values are standardized and never change.
 */
#define CLR_BLACK   0
#define CLR_RED     1
#define CLR_GREEN   2
#define CLR_YELLOW  3
#define CLR_BLUE    4
#define CLR_MAGENTA 5
#define CLR_CYAN    6
#define CLR_WHITE   7

static const struct { const char *name; short value; } color_names[] = {
    { "black",   CLR_BLACK },
    { "red",     CLR_RED },
    { "green",   CLR_GREEN },
    { "yellow",  CLR_YELLOW },
    { "blue",    CLR_BLUE },
    { "magenta", CLR_MAGENTA },
    { "cyan",    CLR_CYAN },
    { "white",   CLR_WHITE },
    { "-1",      -1 },
    { "default", -1 },
    { NULL, 0 }
};

short theme_color_from_name(const char *name)
{
    if (!name || name[0] == '\0') return -2;

    for (int i = 0; color_names[i].name; i++) {
        if (strcmp(name, color_names[i].name) == 0)
            return color_names[i].value;
    }
    return -2;  /* unrecognized */
}

/* ============================================================================
 * Key-to-CPAIR mapping
 * ============================================================================ */

/*
 * Maps theme file key names to CPAIR_* constants.
 * The order doesn't matter — it's searched linearly.
 */
static const struct { const char *key; int cpair; } key_map[] = {
    { "gutter",           CPAIR_GUTTER },
    { "status",           CPAIR_STATUS },
    { "status_dirty",     CPAIR_STATUS_DIRTY },
    { "selection",        CPAIR_SELECTION },
    { "tab_active",       CPAIR_TAB_ACTIVE },
    { "tab_inactive",     CPAIR_TAB_INACTIVE },
    { "search_match",     CPAIR_SEARCH_MATCH },
    { "search_cur",       CPAIR_SEARCH_CUR },
    { "bracket",          CPAIR_BRACKET },
    { "syn_keyword",      CPAIR_SYN_KEYWORD },
    { "syn_type",         CPAIR_SYN_TYPE },
    { "syn_string",       CPAIR_SYN_STRING },
    { "syn_comment",      CPAIR_SYN_COMMENT },
    { "syn_preproc",      CPAIR_SYN_PREPROC },
    { "syn_number",       CPAIR_SYN_NUMBER },
    { "filetree_dir",     CPAIR_FILETREE_DIR },
    { "filetree_file",    CPAIR_FILETREE_FILE },
    { "filetree_cursor",  CPAIR_FILETREE_CURSOR },
    { "region",           CPAIR_REGION },
    { "git_added",        CPAIR_GIT_ADDED },
    { "git_modified",     CPAIR_GIT_MODIFIED },
    { "git_deleted",      CPAIR_GIT_DELETED },
    { "git_panel_cursor", CPAIR_GIT_PANEL_CURSOR },
    { "git_old_line",     CPAIR_GIT_OLD_LINE },
    { "blame",            CPAIR_BLAME },
    { "git_staged",       CPAIR_GIT_STAGED },
    { "build_error",      CPAIR_BUILD_ERROR },
    { "build_warning",    CPAIR_BUILD_WARNING },
    { "build_cursor",     CPAIR_BUILD_CURSOR },
    { "build_success",    CPAIR_BUILD_SUCCESS },
    { "finder_border",    CPAIR_FINDER_BORDER },
    { "finder_cursor",    CPAIR_FINDER_CURSOR },
    { "finder_match",     CPAIR_FINDER_MATCH },
    { NULL, 0 }
};

/* ============================================================================
 * Built-in themes
 * ============================================================================ */

/*
 * Helper macro: define a color pair inline.
 * P(fg, bg, bold) expands to a ThemeColorDef initializer.
 */
#define P(f, b, bold) { (f), (b), (bold) }

/*
 * The default dark theme — matches the hardcoded init_pair() calls that
 * were in display_init() before themes were introduced.  This ensures
 * the editor looks identical to the pre-theme version by default.
 */
static const ThemeColorDef default_dark_pairs[] = {
    /*  0 unused   */ P(0, 0, 0),
    /*  1 DEFAULT  */ P(-1, -1, 0),
    /*  2 GUTTER   */ P(CLR_CYAN, -1, 0),
    /*  3 STATUS   */ P(CLR_BLACK, CLR_CYAN, 0),
    /*  4 DIRTY    */ P(CLR_BLACK, CLR_YELLOW, 0),
    /*  5 CURLINE  */ P(-1, -1, 0),
    /*  6 SELECT   */ P(CLR_BLACK, CLR_CYAN, 0),
    /*  7 TAB_ACT  */ P(CLR_WHITE, CLR_BLUE, 0),
    /*  8 TAB_INA  */ P(CLR_BLACK, CLR_WHITE, 0),
    /*  9 SRCH_M   */ P(CLR_BLACK, CLR_YELLOW, 0),
    /* 10 SRCH_C   */ P(CLR_BLACK, CLR_GREEN, 0),
    /* 11 BRACKET  */ P(CLR_WHITE, CLR_MAGENTA, 1),
    /* 12 SYN_KW   */ P(CLR_YELLOW, -1, 1),
    /* 13 SYN_TY   */ P(CLR_CYAN, -1, 0),
    /* 14 SYN_STR  */ P(CLR_GREEN, -1, 0),
    /* 15 SYN_CMT  */ P(CLR_BLUE, -1, 0),
    /* 16 SYN_PRE  */ P(CLR_MAGENTA, -1, 0),
    /* 17 SYN_NUM  */ P(CLR_RED, -1, 0),
    /* 18 TREE_DIR */ P(CLR_BLUE, -1, 1),
    /* 19 TREE_FIL */ P(-1, -1, 0),
    /* 20 TREE_CUR */ P(CLR_BLACK, CLR_WHITE, 1),
    /* 21 REGION   */ P(CLR_RED, -1, 0),
    /* 22 GIT_ADD  */ P(CLR_GREEN, -1, 0),
    /* 23 GIT_MOD  */ P(CLR_YELLOW, -1, 0),
    /* 24 GIT_DEL  */ P(CLR_RED, -1, 0),
    /* 25 GIT_PCUR */ P(CLR_BLACK, CLR_WHITE, 0),
    /* 26 GIT_OLD  */ P(CLR_RED, -1, 0),
    /* 27 BLAME    */ P(CLR_CYAN, -1, 0),
    /* 28 GIT_STG  */ P(CLR_GREEN, -1, 0),
    /* 29 BLD_ERR  */ P(CLR_RED, -1, 0),
    /* 30 BLD_WRN  */ P(CLR_YELLOW, -1, 0),
    /* 31 BLD_CUR  */ P(CLR_BLACK, CLR_WHITE, 0),
    /* 32 BLD_SUC  */ P(CLR_GREEN, -1, 0),
    /* 33 FND_BRD  */ P(CLR_CYAN, -1, 0),
    /* 34 FND_CUR  */ P(CLR_BLACK, CLR_WHITE, 0),
    /* 35 FND_MCH  */ P(CLR_GREEN, -1, 0),
};

static const ThemeColorDef default_light_pairs[] = {
    P(0, 0, 0),                            /*  0 */
    P(-1, -1, 0),                          /*  1 DEFAULT */
    P(CLR_BLUE, -1, 0),                    /*  2 GUTTER */
    P(CLR_WHITE, CLR_BLUE, 0),             /*  3 STATUS */
    P(CLR_WHITE, CLR_RED, 0),              /*  4 DIRTY */
    P(-1, -1, 0),                          /*  5 CURLINE */
    P(CLR_WHITE, CLR_BLUE, 0),             /*  6 SELECT */
    P(CLR_WHITE, CLR_BLUE, 0),             /*  7 TAB_ACT */
    P(CLR_BLACK, CLR_WHITE, 0),            /*  8 TAB_INA */
    P(CLR_BLACK, CLR_YELLOW, 0),           /*  9 SRCH_M */
    P(CLR_BLACK, CLR_GREEN, 0),            /* 10 SRCH_C */
    P(CLR_WHITE, CLR_MAGENTA, 1),          /* 11 BRACKET */
    P(CLR_BLUE, -1, 1),                    /* 12 SYN_KW */
    P(CLR_MAGENTA, -1, 0),                 /* 13 SYN_TY */
    P(CLR_RED, -1, 0),                     /* 14 SYN_STR */
    P(CLR_GREEN, -1, 0),                   /* 15 SYN_CMT */
    P(CLR_MAGENTA, -1, 1),                 /* 16 SYN_PRE */
    P(CLR_CYAN, -1, 0),                    /* 17 SYN_NUM */
    P(CLR_BLUE, -1, 1),                    /* 18 TREE_DIR */
    P(-1, -1, 0),                          /* 19 TREE_FIL */
    P(CLR_WHITE, CLR_BLUE, 1),             /* 20 TREE_CUR */
    P(CLR_RED, -1, 0),                     /* 21 REGION */
    P(CLR_GREEN, -1, 0),                   /* 22 GIT_ADD */
    P(CLR_YELLOW, -1, 0),                  /* 23 GIT_MOD */
    P(CLR_RED, -1, 0),                     /* 24 GIT_DEL */
    P(CLR_WHITE, CLR_BLUE, 0),             /* 25 GIT_PCUR */
    P(CLR_RED, -1, 0),                     /* 26 GIT_OLD */
    P(CLR_BLUE, -1, 0),                    /* 27 BLAME */
    P(CLR_GREEN, -1, 0),                   /* 28 GIT_STG */
    P(CLR_RED, -1, 0),                     /* 29 BLD_ERR */
    P(CLR_YELLOW, -1, 0),                  /* 30 BLD_WRN */
    P(CLR_WHITE, CLR_BLUE, 0),             /* 31 BLD_CUR */
    P(CLR_GREEN, -1, 0),                   /* 32 BLD_SUC */
    P(CLR_BLUE, -1, 0),                    /* 33 FND_BRD */
    P(CLR_WHITE, CLR_BLUE, 0),             /* 34 FND_CUR */
    P(CLR_GREEN, -1, 0),                   /* 35 FND_MCH */
};

static const ThemeColorDef monokai_pairs[] = {
    P(0, 0, 0),                            /*  0 */
    P(-1, -1, 0),                          /*  1 DEFAULT */
    P(CLR_WHITE, -1, 0),                   /*  2 GUTTER */
    P(CLR_BLACK, CLR_GREEN, 0),            /*  3 STATUS */
    P(CLR_BLACK, CLR_YELLOW, 0),           /*  4 DIRTY */
    P(-1, -1, 0),                          /*  5 CURLINE */
    P(CLR_BLACK, CLR_YELLOW, 0),           /*  6 SELECT */
    P(CLR_BLACK, CLR_GREEN, 0),            /*  7 TAB_ACT */
    P(CLR_WHITE, -1, 0),                   /*  8 TAB_INA */
    P(CLR_BLACK, CLR_YELLOW, 0),           /*  9 SRCH_M */
    P(CLR_BLACK, CLR_GREEN, 0),            /* 10 SRCH_C */
    P(CLR_BLACK, CLR_YELLOW, 1),           /* 11 BRACKET */
    P(CLR_RED, -1, 1),                     /* 12 SYN_KW  (monokai pink ≈ red) */
    P(CLR_CYAN, -1, 0),                    /* 13 SYN_TY  (monokai blue ≈ cyan) */
    P(CLR_YELLOW, -1, 0),                  /* 14 SYN_STR (monokai yellow) */
    P(CLR_WHITE, -1, 0),                   /* 15 SYN_CMT (monokai grey ≈ white dim) */
    P(CLR_RED, -1, 0),                     /* 16 SYN_PRE */
    P(CLR_MAGENTA, -1, 0),                 /* 17 SYN_NUM (monokai purple ≈ magenta) */
    P(CLR_BLUE, -1, 1),                    /* 18 TREE_DIR */
    P(-1, -1, 0),                          /* 19 TREE_FIL */
    P(CLR_BLACK, CLR_GREEN, 1),            /* 20 TREE_CUR */
    P(CLR_RED, -1, 0),                     /* 21 REGION */
    P(CLR_GREEN, -1, 0),                   /* 22-28: git (same as default) */
    P(CLR_YELLOW, -1, 0),
    P(CLR_RED, -1, 0),
    P(CLR_BLACK, CLR_WHITE, 0),
    P(CLR_RED, -1, 0),
    P(CLR_CYAN, -1, 0),
    P(CLR_GREEN, -1, 0),
    P(CLR_RED, -1, 0),                     /* 29-35: build/finder (same) */
    P(CLR_YELLOW, -1, 0),
    P(CLR_BLACK, CLR_WHITE, 0),
    P(CLR_GREEN, -1, 0),
    P(CLR_CYAN, -1, 0),
    P(CLR_BLACK, CLR_WHITE, 0),
    P(CLR_GREEN, -1, 0),
};

/*
 * Gruvbox Dark — warm retro theme.
 *
 * The original Gruvbox uses 256/truecolor, but we approximate with 8 colors.
 * The key characteristic is a warm dark background with earthy tones:
 *   keywords = red, types = yellow, strings = green, comments = cyan,
 *   numbers = magenta, preprocessor = cyan.
 *
 * default_bg = BLACK gives the dark background feel.  Actual Gruvbox bg
 * is #282828 (dark brown-grey) — black is the closest 8-color approximation.
 */
static const ThemeColorDef gruvbox_pairs[] = {
    P(0, 0, 0),                            /*  0 */
    P(-1, -1, 0),                          /*  1 DEFAULT */
    P(CLR_YELLOW, -1, 0),                  /*  2 GUTTER (warm gutter) */
    P(CLR_BLACK, CLR_YELLOW, 0),           /*  3 STATUS (gruvbox yellow bar) */
    P(CLR_BLACK, CLR_RED, 0),              /*  4 DIRTY (red = unsaved) */
    P(-1, -1, 0),                          /*  5 CURLINE */
    P(CLR_BLACK, CLR_YELLOW, 0),           /*  6 SELECT */
    P(CLR_BLACK, CLR_GREEN, 0),            /*  7 TAB_ACT (green active tab) */
    P(CLR_WHITE, -1, 0),                   /*  8 TAB_INA */
    P(CLR_BLACK, CLR_YELLOW, 0),           /*  9 SRCH_M */
    P(CLR_BLACK, CLR_GREEN, 0),            /* 10 SRCH_C */
    P(CLR_BLACK, CLR_YELLOW, 1),           /* 11 BRACKET */
    P(CLR_RED, -1, 1),                     /* 12 SYN_KW  (gruvbox red) */
    P(CLR_YELLOW, -1, 0),                  /* 13 SYN_TY  (gruvbox yellow) */
    P(CLR_GREEN, -1, 0),                   /* 14 SYN_STR (gruvbox green) */
    P(CLR_CYAN, -1, 0),                    /* 15 SYN_CMT (gruvbox grey ≈ cyan) */
    P(CLR_CYAN, -1, 0),                    /* 16 SYN_PRE (gruvbox aqua) */
    P(CLR_MAGENTA, -1, 0),                 /* 17 SYN_NUM (gruvbox purple) */
    P(CLR_GREEN, -1, 1),                   /* 18 TREE_DIR */
    P(-1, -1, 0),                          /* 19 TREE_FIL */
    P(CLR_BLACK, CLR_YELLOW, 1),           /* 20 TREE_CUR */
    P(CLR_RED, -1, 0),                     /* 21 REGION */
    P(CLR_GREEN, -1, 0),                   /* 22 GIT_ADD */
    P(CLR_YELLOW, -1, 0),                  /* 23 GIT_MOD */
    P(CLR_RED, -1, 0),                     /* 24 GIT_DEL */
    P(CLR_BLACK, CLR_WHITE, 0),            /* 25 GIT_PCUR */
    P(CLR_RED, -1, 0),                     /* 26 GIT_OLD */
    P(CLR_CYAN, -1, 0),                    /* 27 BLAME */
    P(CLR_GREEN, -1, 0),                   /* 28 GIT_STG */
    P(CLR_RED, -1, 0),                     /* 29 BLD_ERR */
    P(CLR_YELLOW, -1, 0),                  /* 30 BLD_WRN */
    P(CLR_BLACK, CLR_WHITE, 0),            /* 31 BLD_CUR */
    P(CLR_GREEN, -1, 0),                   /* 32 BLD_SUC */
    P(CLR_YELLOW, -1, 0),                  /* 33 FND_BRD (warm yellow border) */
    P(CLR_BLACK, CLR_YELLOW, 0),           /* 34 FND_CUR */
    P(CLR_GREEN, -1, 0),                   /* 35 FND_MCH */
};

#undef P

/* ============================================================================
 * Lifecycle
 * ============================================================================ */

/*
 * copy_theme — copy a built-in theme definition into a Theme struct.
 */
static void copy_theme(Theme *out, const char *name,
                       const ThemeColorDef *pairs, int num_pairs,
                       short default_fg, short default_bg)
{
    strncpy(out->name, name, THEME_NAME_MAX - 1);
    out->name[THEME_NAME_MAX - 1] = '\0';

    /* Start with all defaults (terminal default colors, no bold) */
    memset(out->pairs, 0, sizeof(out->pairs));

    /* Copy the built-in pairs (1-indexed) */
    int count = num_pairs;
    if (count > THEME_NUM_PAIRS) count = THEME_NUM_PAIRS;
    for (int i = 0; i <= count; i++)
        out->pairs[i] = pairs[i];

    out->default_fg = default_fg;
    out->default_bg = default_bg;
}

void theme_init(ThemeManager *tm)
{
    memset(tm, 0, sizeof(ThemeManager));

    /* -1, -1 = keep the terminal's own default fg/bg */
    copy_theme(&tm->themes[0], "Default Dark",
               default_dark_pairs, 35, -1, -1);
    copy_theme(&tm->themes[1], "Default Light",
               default_light_pairs, 35, -1, -1);
    copy_theme(&tm->themes[2], "Monokai",
               monokai_pairs, 35, CLR_WHITE, CLR_BLACK);
    copy_theme(&tm->themes[3], "Gruvbox Dark",
               gruvbox_pairs, 35, CLR_WHITE, CLR_BLACK);

    tm->count        = 4;
    tm->active_index = 0;
}

/* ============================================================================
 * Theme selection
 * ============================================================================ */

const char *theme_cycle(ThemeManager *tm)
{
    if (!tm || tm->count == 0) return "???";
    tm->active_index = (tm->active_index + 1) % tm->count;
    return tm->themes[tm->active_index].name;
}

int theme_set_by_name(ThemeManager *tm, const char *name)
{
    if (!tm || !name) return -1;
    for (int i = 0; i < tm->count; i++) {
        if (strcmp(tm->themes[i].name, name) == 0) {
            tm->active_index = i;
            return 0;
        }
    }
    return -1;
}

const Theme *theme_active(const ThemeManager *tm)
{
    if (!tm || tm->count == 0) return NULL;
    return &tm->themes[tm->active_index];
}

/* ============================================================================
 * Parsing — .theme file format
 * ============================================================================ */

/*
 * trim — skip leading whitespace and return a pointer into the string.
 * Does not modify the original string.
 */
static const char *trim(const char *s)
{
    while (*s == ' ' || *s == '\t') s++;
    return s;
}

int theme_parse(Theme *out, const char *text)
{
    if (!out || !text) return -1;

    /*
     * Start from a copy of the default dark theme so unspecified keys
     * retain sensible defaults.
     */
    copy_theme(out, "Custom", default_dark_pairs, 35, -1, -1);

    const char *p = text;
    while (*p) {
        /* Find end of line */
        const char *eol = p;
        while (*eol && *eol != '\n') eol++;

        /* Copy line to a local buffer for easy parsing */
        int line_len = (int)(eol - p);
        char line[512];
        if (line_len >= (int)sizeof(line)) line_len = (int)sizeof(line) - 1;
        memcpy(line, p, line_len);
        line[line_len] = '\0';

        /* Advance past newline */
        p = (*eol == '\n') ? eol + 1 : eol;

        /* Skip comments and blank lines */
        const char *trimmed = trim(line);
        if (trimmed[0] == '#' || trimmed[0] == '\0') continue;

        /* Parse "name = value" for the theme name */
        if (strncmp(trimmed, "name", 4) == 0) {
            const char *eq = strchr(trimmed, '=');
            if (eq) {
                const char *val = trim(eq + 1);
                /* Strip trailing whitespace */
                int vlen = (int)strlen(val);
                while (vlen > 0 && (val[vlen-1] == ' ' || val[vlen-1] == '\t'
                       || val[vlen-1] == '\n' || val[vlen-1] == '\r'))
                    vlen--;
                if (vlen >= THEME_NAME_MAX) vlen = THEME_NAME_MAX - 1;
                memcpy(out->name, val, vlen);
                out->name[vlen] = '\0';
            }
            continue;
        }

        /* Parse "key = fg [bg]" */
        char *eq = strchr(line, '=');
        if (!eq) continue;

        *eq = '\0';  /* split into key and value */
        const char *key = trim(line);
        const char *val = trim(eq + 1);

        /* Trim trailing whitespace from key */
        char key_buf[64];
        strncpy(key_buf, key, sizeof(key_buf) - 1);
        key_buf[sizeof(key_buf) - 1] = '\0';
        int klen = (int)strlen(key_buf);
        while (klen > 0 && (key_buf[klen-1] == ' ' || key_buf[klen-1] == '\t'))
            key_buf[--klen] = '\0';

        /* Handle default_fg and default_bg keys */
        if (strcmp(key_buf, "default_fg") == 0) {
            char color_name[32] = "";
            sscanf(val, "%31s", color_name);
            short c = theme_color_from_name(color_name);
            if (c != -2) out->default_fg = c;
            continue;
        }
        if (strcmp(key_buf, "default_bg") == 0) {
            char color_name[32] = "";
            sscanf(val, "%31s", color_name);
            short c = theme_color_from_name(color_name);
            if (c != -2) out->default_bg = c;
            continue;
        }

        /* Look up the CPAIR for this key */
        int cpair = -1;
        for (int i = 0; key_map[i].key; i++) {
            if (strcmp(key_buf, key_map[i].key) == 0) {
                cpair = key_map[i].cpair;
                break;
            }
        }

        /* Check for bold suffix: "syn_keyword_bold = 1" */
        int is_bold_key = 0;
        if (cpair < 0 && klen > 5
                && strcmp(key_buf + klen - 5, "_bold") == 0) {
            key_buf[klen - 5] = '\0';
            for (int i = 0; key_map[i].key; i++) {
                if (strcmp(key_buf, key_map[i].key) == 0) {
                    cpair = key_map[i].cpair;
                    is_bold_key = 1;
                    break;
                }
            }
        }

        if (cpair < 0 || cpair > THEME_NUM_PAIRS) continue;

        if (is_bold_key) {
            out->pairs[cpair].bold = (val[0] == '1') ? 1 : 0;
            continue;
        }

        /* Parse foreground color */
        char fg_name[32] = "", bg_name[32] = "";
        sscanf(val, "%31s %31s", fg_name, bg_name);

        short fg = theme_color_from_name(fg_name);
        if (fg == -2) continue;  /* unrecognized — skip this line */

        out->pairs[cpair].fg = fg;

        /* Background is optional (defaults to -1) */
        if (bg_name[0] != '\0') {
            short bg = theme_color_from_name(bg_name);
            if (bg != -2)
                out->pairs[cpair].bg = bg;
        }
    }

    return 0;
}

/* ============================================================================
 * Disk loading
 * ============================================================================ */

void theme_load_from_disk(ThemeManager *tm)
{
    if (!tm) return;

    const char *home = getenv("HOME");
    if (!home) return;

    char dir_path[1024];
    snprintf(dir_path, sizeof(dir_path), "%s/.config/texty/themes", home);

    DIR *dp = opendir(dir_path);
    if (!dp) return;

    struct dirent *ent;
    while ((ent = readdir(dp)) != NULL && tm->count < THEME_MAX_THEMES) {
        /* Only load .theme files */
        int nlen = (int)strlen(ent->d_name);
        if (nlen < 7 || strcmp(ent->d_name + nlen - 6, ".theme") != 0)
            continue;

        char filepath[2048];
        snprintf(filepath, sizeof(filepath), "%s/%s", dir_path, ent->d_name);

        /* Read the file */
        FILE *fp = fopen(filepath, "r");
        if (!fp) continue;

        fseek(fp, 0, SEEK_END);
        long size = ftell(fp);
        fseek(fp, 0, SEEK_SET);
        if (size <= 0 || size > 8192) { fclose(fp); continue; }

        char *text = malloc(size + 1);
        if (!text) { fclose(fp); continue; }
        int read = (int)fread(text, 1, size, fp);
        fclose(fp);
        text[read] = '\0';

        /* Parse the theme */
        Theme *t = &tm->themes[tm->count];
        if (theme_parse(t, text) == 0 && t->name[0] != '\0')
            tm->count++;

        free(text);
    }

    closedir(dp);
}

/* ============================================================================
 * Config integration — texty.json
 * ============================================================================ */

void theme_parse_config(char *out_name, int out_size, const char *json_text)
{
    out_name[0] = '\0';
    if (!json_text || json_text[0] == '\0') return;

    /*
     * Simple manual JSON parsing — find "theme" key and extract string value.
     * Same pattern as build_parse_config() in build.c.
     */
    const char *key = strstr(json_text, "\"theme\"");
    if (!key) return;

    const char *p = key + 7;  /* strlen("\"theme\"") = 7 */
    while (*p && *p != ':') p++;
    if (!*p) return;
    p++;

    while (*p && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;

    if (*p != '"') return;
    p++;

    const char *start = p;
    while (*p && *p != '"') {
        if (*p == '\\' && p[1] == '"') p++;
        p++;
    }

    int value_len = (int)(p - start);
    if (value_len >= out_size) value_len = out_size - 1;
    if (value_len > 0)
        memcpy(out_name, start, value_len);
    out_name[value_len] = '\0';
}
