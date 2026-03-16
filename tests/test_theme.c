/*
 * test_theme.c — Unit tests for theme.c
 * =============================================================================
 * Tests the pure-logic theme functions: color name lookup, theme file
 * parsing, config parsing, initialization, and theme selection.
 * =============================================================================
 */

#include "test_runner.h"
#include "theme.h"

#include <stdlib.h>
#include <string.h>

/* ============================================================================
 * Color name lookup
 * ============================================================================ */

TEST(test_color_from_name_basic)
{
    ASSERT(theme_color_from_name("black")   == 0, "black = 0");
    ASSERT(theme_color_from_name("red")     == 1, "red = 1");
    ASSERT(theme_color_from_name("green")   == 2, "green = 2");
    ASSERT(theme_color_from_name("yellow")  == 3, "yellow = 3");
    ASSERT(theme_color_from_name("blue")    == 4, "blue = 4");
    ASSERT(theme_color_from_name("magenta") == 5, "magenta = 5");
    ASSERT(theme_color_from_name("cyan")    == 6, "cyan = 6");
    ASSERT(theme_color_from_name("white")   == 7, "white = 7");
}

TEST(test_color_from_name_default)
{
    ASSERT(theme_color_from_name("-1")      == -1, "-1 = terminal default");
    ASSERT(theme_color_from_name("default") == -1, "default = terminal default");
}

TEST(test_color_from_name_invalid)
{
    ASSERT(theme_color_from_name("purple")  == -2, "unrecognized returns -2");
    ASSERT(theme_color_from_name("")        == -2, "empty returns -2");
    ASSERT(theme_color_from_name(NULL)      == -2, "NULL returns -2");
}

/* ============================================================================
 * Theme initialization
 * ============================================================================ */

TEST(test_theme_init_builtin_count)
{
    ThemeManager tm;
    theme_init(&tm);

    ASSERT(tm.count >= 4, "at least 4 built-in themes");
    ASSERT(tm.active_index == 0, "default theme is index 0");
    ASSERT(strcmp(tm.themes[0].name, "Default Dark") == 0,
           "first theme is Default Dark");
    ASSERT(strcmp(tm.themes[3].name, "Gruvbox Dark") == 0,
           "fourth theme is Gruvbox Dark");
}

TEST(test_theme_init_default_dark_colors)
{
    ThemeManager tm;
    theme_init(&tm);

    /* Verify a few key color pairs match the expected defaults */
    const Theme *t = &tm.themes[0];
    ASSERT(t->pairs[2].fg == 6,  "GUTTER fg = cyan (6)");
    ASSERT(t->pairs[2].bg == -1, "GUTTER bg = default");
    ASSERT(t->pairs[12].fg == 3, "SYN_KEYWORD fg = yellow (3)");
    ASSERT(t->pairs[12].bold == 1, "SYN_KEYWORD is bold");
    ASSERT(t->pairs[14].fg == 2, "SYN_STRING fg = green (2)");
}

/* ============================================================================
 * Theme cycling and selection
 * ============================================================================ */

TEST(test_theme_cycle)
{
    ThemeManager tm;
    theme_init(&tm);

    ASSERT(tm.active_index == 0, "start at 0");

    const char *name = theme_cycle(&tm);
    ASSERT(tm.active_index == 1, "cycled to 1");
    ASSERT(name != NULL, "cycle returns name");

    /* Cycle through all and wrap around */
    for (int i = 0; i < tm.count; i++)
        theme_cycle(&tm);
    /* Should be back to index 1 (we cycled count+1 times total) */
    ASSERT(tm.active_index == 1, "wraps around");
}

TEST(test_theme_set_by_name)
{
    ThemeManager tm;
    theme_init(&tm);

    ASSERT(theme_set_by_name(&tm, "Monokai") == 0, "found Monokai");
    ASSERT(tm.active_index == 2, "Monokai is index 2");

    ASSERT(theme_set_by_name(&tm, "Gruvbox Dark") == 0, "found Gruvbox");
    ASSERT(tm.active_index == 3, "Gruvbox is index 3");

    ASSERT(theme_set_by_name(&tm, "Nonexistent") == -1, "not found");
    ASSERT(tm.active_index == 3, "index unchanged after not found");
}

TEST(test_theme_default_bg)
{
    ThemeManager tm;
    theme_init(&tm);

    /* Default Dark keeps terminal defaults */
    ASSERT(tm.themes[0].default_fg == -1, "dark: default_fg = -1");
    ASSERT(tm.themes[0].default_bg == -1, "dark: default_bg = -1");

    /* Gruvbox forces black background */
    ASSERT(tm.themes[3].default_bg == 0, "gruvbox: default_bg = black (0)");
    ASSERT(tm.themes[3].default_fg == 7, "gruvbox: default_fg = white (7)");
}

TEST(test_parse_default_bg)
{
    const char *text =
        "name = Custom BG\n"
        "default_fg = white\n"
        "default_bg = blue\n";

    Theme t;
    theme_parse(&t, text);
    ASSERT(t.default_fg == 7, "parsed default_fg = white");
    ASSERT(t.default_bg == 4, "parsed default_bg = blue");
}

TEST(test_theme_active)
{
    ThemeManager tm;
    theme_init(&tm);

    const Theme *t = theme_active(&tm);
    ASSERT(t != NULL, "active theme not NULL");
    ASSERT(strcmp(t->name, "Default Dark") == 0, "active is Default Dark");
}

/* ============================================================================
 * Theme file parsing
 * ============================================================================ */

TEST(test_parse_minimal_theme)
{
    const char *text =
        "name = My Theme\n"
        "syn_keyword = red -1\n"
        "syn_string = green -1\n";

    Theme t;
    int result = theme_parse(&t, text);
    ASSERT(result == 0, "parse succeeds");
    ASSERT(strcmp(t.name, "My Theme") == 0, "name parsed");

    /* CPAIR_SYN_KEYWORD = 12 */
    ASSERT(t.pairs[12].fg == 1, "keyword fg = red (1)");
    ASSERT(t.pairs[12].bg == -1, "keyword bg = default");

    /* CPAIR_SYN_STRING = 14 */
    ASSERT(t.pairs[14].fg == 2, "string fg = green (2)");
}

TEST(test_parse_with_comments_and_blanks)
{
    const char *text =
        "# This is a comment\n"
        "\n"
        "name = Commented Theme\n"
        "\n"
        "# Another comment\n"
        "gutter = blue -1\n";

    Theme t;
    theme_parse(&t, text);
    ASSERT(strcmp(t.name, "Commented Theme") == 0, "name parsed");
    ASSERT(t.pairs[2].fg == 4, "gutter fg = blue (4)");
}

TEST(test_parse_unrecognized_keys_skipped)
{
    const char *text =
        "name = Test\n"
        "unknown_key = red\n"
        "syn_type = magenta -1\n";

    Theme t;
    int result = theme_parse(&t, text);
    ASSERT(result == 0, "parse succeeds despite unknown key");
    /* CPAIR_SYN_TYPE = 13 */
    ASSERT(t.pairs[13].fg == 5, "syn_type fg = magenta (5)");
}

TEST(test_parse_bold_attribute)
{
    const char *text =
        "name = Bold Test\n"
        "syn_comment = blue -1\n"
        "syn_comment_bold = 1\n";

    Theme t;
    theme_parse(&t, text);
    /* CPAIR_SYN_COMMENT = 15 */
    ASSERT(t.pairs[15].fg == 4, "comment fg = blue");
    ASSERT(t.pairs[15].bold == 1, "comment is bold");
}

TEST(test_parse_defaults_from_dark)
{
    /*
     * A theme that only sets one key should inherit all other
     * values from Default Dark.
     */
    const char *text = "name = Sparse\nsyn_keyword = white -1\n";

    Theme t;
    theme_parse(&t, text);
    /* SYN_KEYWORD overridden */
    ASSERT(t.pairs[12].fg == 7, "keyword changed to white");
    /* SYN_STRING should still be green (default dark value) */
    ASSERT(t.pairs[14].fg == 2, "string kept default green");
}

TEST(test_parse_null_input)
{
    Theme t;
    ASSERT(theme_parse(&t, NULL) == -1, "NULL input returns -1");
}

/* ============================================================================
 * Config parsing — texty.json
 * ============================================================================ */

TEST(test_config_parse_theme_key)
{
    char name[64];
    theme_parse_config(name, sizeof(name),
                       "{ \"build_command\": \"make\", \"theme\": \"Monokai\" }");
    ASSERT(strcmp(name, "Monokai") == 0, "theme name extracted");
}

TEST(test_config_parse_no_theme_key)
{
    char name[64];
    theme_parse_config(name, sizeof(name),
                       "{ \"build_command\": \"make\" }");
    ASSERT(name[0] == '\0', "no theme key → empty string");
}

TEST(test_config_parse_null)
{
    char name[64];
    theme_parse_config(name, sizeof(name), NULL);
    ASSERT(name[0] == '\0', "NULL → empty string");
}

/* ============================================================================
 * main
 * ============================================================================ */

int main(void)
{
    printf("=== test_theme ===\n");

    /* Color name lookup */
    RUN(test_color_from_name_basic);
    RUN(test_color_from_name_default);
    RUN(test_color_from_name_invalid);

    /* Initialization */
    RUN(test_theme_init_builtin_count);
    RUN(test_theme_init_default_dark_colors);

    /* Cycling and selection */
    RUN(test_theme_cycle);
    RUN(test_theme_set_by_name);
    RUN(test_theme_active);

    /* Theme file parsing */
    RUN(test_parse_minimal_theme);
    RUN(test_parse_with_comments_and_blanks);
    RUN(test_parse_unrecognized_keys_skipped);
    RUN(test_parse_bold_attribute);
    RUN(test_parse_defaults_from_dark);
    RUN(test_parse_null_input);

    /* Default background */
    RUN(test_theme_default_bg);
    RUN(test_parse_default_bg);

    /* Config parsing */
    RUN(test_config_parse_theme_key);
    RUN(test_config_parse_no_theme_key);
    RUN(test_config_parse_null);

    TEST_SUMMARY();
}
