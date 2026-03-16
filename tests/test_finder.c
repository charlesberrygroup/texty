/*
 * test_finder.c — Unit tests for finder.c
 * =============================================================================
 * Tests fuzzy matching, scoring, filtering, and file collection without
 * running ncurses.  The key functions under test are finder_fuzzy_score()
 * and finder_filter().
 * =============================================================================
 */

/*
 * _DARWIN_C_SOURCE exposes mkdtemp() on macOS, which is hidden when
 * _POSIX_C_SOURCE is set.  Must come before ALL includes.
 */
#define _DARWIN_C_SOURCE

#include "test_runner.h"
#include "finder.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

/* ============================================================================
 * Fuzzy score tests
 * ============================================================================ */

TEST(test_score_empty_query)
{
    /* Empty query matches everything with base score */
    ASSERT(finder_fuzzy_score("", "main.c") > 0,
           "empty query matches anything");
}

TEST(test_score_exact_match)
{
    int exact = finder_fuzzy_score("main.c", "main.c");
    int partial = finder_fuzzy_score("mc", "main.c");
    ASSERT(exact > partial, "exact match scores higher than partial");
}

TEST(test_score_subsequence_match)
{
    ASSERT(finder_fuzzy_score("mc", "main.c") > 0,
           "subsequence 'mc' matches 'main.c'");
    ASSERT(finder_fuzzy_score("edc", "editor.c") > 0,
           "subsequence 'edc' matches 'editor.c'");
}

TEST(test_score_no_match)
{
    ASSERT(finder_fuzzy_score("xyz", "main.c") == 0,
           "non-matching query returns 0");
    ASSERT(finder_fuzzy_score("cm", "main.c") == 0,
           "'cm' does not match 'main.c' (wrong order)");
}

TEST(test_score_case_insensitive)
{
    ASSERT(finder_fuzzy_score("MC", "main.c") > 0,
           "uppercase query matches lowercase text");
    ASSERT(finder_fuzzy_score("main", "MAIN.C") > 0,
           "lowercase query matches uppercase text");
}

TEST(test_score_consecutive_bonus)
{
    /*
     * "main" should score higher on "main.c" (4 consecutive matches)
     * than on "mxaxixn.c" (matches scattered with non-separator chars).
     */
    int consecutive = finder_fuzzy_score("main", "main.c");
    int scattered   = finder_fuzzy_score("main", "mxaxixn.c");
    ASSERT(consecutive > scattered,
           "consecutive matches score higher than scattered");
}

TEST(test_score_separator_bonus)
{
    /*
     * "ec" should score higher on "editor.c" (e at start, c after '.')
     * than on "ovec.c" (both mid-word).
     */
    int with_boundary = finder_fuzzy_score("ec", "editor.c");
    int mid_word      = finder_fuzzy_score("ec", "abcovec.c");
    ASSERT(with_boundary > mid_word,
           "matches at word boundaries score higher");
}

TEST(test_score_start_bonus)
{
    /*
     * Query starting at position 0 of text gets a start bonus.
     */
    int at_start = finder_fuzzy_score("m", "main.c");
    int mid_text = finder_fuzzy_score("m", "xxmain.c");
    ASSERT(at_start > mid_text,
           "match at start of text scores higher");
}

TEST(test_score_path_match)
{
    /* Matching a path component — "em" in "editor/main.c" */
    ASSERT(finder_fuzzy_score("em", "editor/main.c") > 0,
           "matches across path separators");
}

TEST(test_score_null_inputs)
{
    ASSERT(finder_fuzzy_score(NULL, "main.c") == 0, "NULL query");
    ASSERT(finder_fuzzy_score("mc", NULL) == 0,     "NULL text");
    ASSERT(finder_fuzzy_score(NULL, NULL) == 0,      "both NULL");
}

TEST(test_score_query_longer_than_text)
{
    ASSERT(finder_fuzzy_score("longquery", "mc") == 0,
           "query longer than text returns 0");
}

/* ============================================================================
 * Filter tests
 * ============================================================================ */

/* Helper: build a small file list for filter tests */
static void setup_files(FinderFile *files, int *count)
{
    *count = 5;
    strcpy(files[0].display, "src/main.c");
    strcpy(files[0].path,    "/project/src/main.c");
    strcpy(files[1].display, "src/editor.c");
    strcpy(files[1].path,    "/project/src/editor.c");
    strcpy(files[2].display, "src/display.c");
    strcpy(files[2].path,    "/project/src/display.c");
    strcpy(files[3].display, "Makefile");
    strcpy(files[3].path,    "/project/Makefile");
    strcpy(files[4].display, "README.md");
    strcpy(files[4].path,    "/project/README.md");
}

TEST(test_filter_empty_query)
{
    FinderFile files[5];
    int count;
    setup_files(files, &count);

    FinderResult results[10];
    int n = finder_filter(files, count, "", results, 10);
    ASSERT(n == 5, "empty query returns all files");
}

TEST(test_filter_matching_query)
{
    FinderFile files[5];
    int count;
    setup_files(files, &count);

    FinderResult results[10];
    int n = finder_filter(files, count, "main", results, 10);
    ASSERT(n >= 1, "at least one match for 'main'");

    /* The top result should be main.c */
    ASSERT(results[0].index == 0, "main.c is top result");
}

TEST(test_filter_no_match)
{
    FinderFile files[5];
    int count;
    setup_files(files, &count);

    FinderResult results[10];
    int n = finder_filter(files, count, "zzzzz", results, 10);
    ASSERT(n == 0, "no matches for 'zzzzz'");
}

TEST(test_filter_sorted_by_score)
{
    FinderFile files[5];
    int count;
    setup_files(files, &count);

    FinderResult results[10];
    int n = finder_filter(files, count, "mc", results, 10);

    /* Results should be sorted by descending score */
    for (int i = 1; i < n; i++)
        ASSERT(results[i - 1].score >= results[i].score,
               "results sorted by descending score");
}

TEST(test_filter_max_results)
{
    FinderFile files[5];
    int count;
    setup_files(files, &count);

    FinderResult results[2];
    int n = finder_filter(files, count, "", results, 2);
    ASSERT(n == 2, "respects max_results limit");
}

TEST(test_filter_null_inputs)
{
    FinderResult results[10];
    ASSERT(finder_filter(NULL, 5, "q", results, 10) == 0, "NULL files");
    ASSERT(finder_filter(NULL, 0, "q", results, 10) == 0, "zero files");
}

/* ============================================================================
 * File collection tests (using temp directories)
 * ============================================================================ */

TEST(test_collect_basic)
{
    /* Create a temp directory with a few files */
    char tmpdir[] = "/tmp/texty_test_XXXXXX";
    if (!mkdtemp(tmpdir)) {
        ASSERT(0, "mkdtemp failed");
        return;
    }

    /* Create files */
    char path1[256], path2[256], subdir[256], path3[256];
    snprintf(path1, sizeof(path1), "%s/file1.c", tmpdir);
    snprintf(subdir, sizeof(subdir), "%s/sub", tmpdir);
    snprintf(path2, sizeof(path2), "%s/file2.h", tmpdir);
    snprintf(path3, sizeof(path3), "%s/file3.c", subdir);

    mkdir(subdir, 0755);
    fclose(fopen(path1, "w"));
    fclose(fopen(path2, "w"));
    fclose(fopen(path3, "w"));

    FinderFile files[64];
    int n = finder_collect_files(tmpdir, files, 64);
    ASSERT(n >= 3, "collected at least 3 files");

    /* Verify our specific files are present */
    int found1 = 0, found2 = 0, found3 = 0;
    for (int i = 0; i < n; i++) {
        ASSERT(files[i].display[0] != '\0', "display path not empty");
        if (strstr(files[i].display, "file1.c")) found1 = 1;
        if (strstr(files[i].display, "file2.h")) found2 = 1;
        if (strstr(files[i].display, "file3.c")) found3 = 1;
    }
    ASSERT(found1, "file1.c found");
    ASSERT(found2, "file2.h found");
    ASSERT(found3, "file3.c in subdir found");

    /* Cleanup */
    unlink(path1);
    unlink(path2);
    unlink(path3);
    rmdir(subdir);
    rmdir(tmpdir);
}

TEST(test_collect_skips_hidden)
{
    char tmpdir[] = "/tmp/texty_test_XXXXXX";
    if (!mkdtemp(tmpdir)) {
        ASSERT(0, "mkdtemp failed");
        return;
    }

    /* Create a visible file and a hidden file */
    char visible[256], hidden[256], hiddendir[256], hiddenfile[256];
    snprintf(visible, sizeof(visible), "%s/visible.c", tmpdir);
    snprintf(hidden, sizeof(hidden), "%s/.hidden", tmpdir);
    snprintf(hiddendir, sizeof(hiddendir), "%s/.hiddendir", tmpdir);
    snprintf(hiddenfile, sizeof(hiddenfile), "%s/.hiddendir/file.c", tmpdir);

    mkdir(hiddendir, 0755);
    fclose(fopen(visible, "w"));
    fclose(fopen(hidden, "w"));
    fclose(fopen(hiddenfile, "w"));

    FinderFile files[64];
    int n = finder_collect_files(tmpdir, files, 64);
    ASSERT(n == 1, "only visible file collected");
    ASSERT(strstr(files[0].display, "visible.c") != NULL,
           "visible.c is the collected file");

    /* Cleanup */
    unlink(visible);
    unlink(hidden);
    unlink(hiddenfile);
    rmdir(hiddendir);
    rmdir(tmpdir);
}

TEST(test_collect_skips_obj_dir)
{
    char tmpdir[] = "/tmp/texty_test_XXXXXX";
    if (!mkdtemp(tmpdir)) {
        ASSERT(0, "mkdtemp failed");
        return;
    }

    char srcfile[256], objdir[256], objfile[256];
    snprintf(srcfile, sizeof(srcfile), "%s/main.c", tmpdir);
    snprintf(objdir, sizeof(objdir), "%s/obj", tmpdir);
    snprintf(objfile, sizeof(objfile), "%s/obj/main.o", tmpdir);

    mkdir(objdir, 0755);
    fclose(fopen(srcfile, "w"));
    fclose(fopen(objfile, "w"));

    FinderFile files[64];
    int n = finder_collect_files(tmpdir, files, 64);
    ASSERT(n == 1, "obj/ directory skipped");

    /* Cleanup */
    unlink(srcfile);
    unlink(objfile);
    rmdir(objdir);
    rmdir(tmpdir);
}

/* ============================================================================
 * Symbol extraction
 * ============================================================================ */

TEST(test_symbols_c_function)
{
    const char *lines[] = {
        "#include <stdio.h>",
        "",
        "int main(int argc, char **argv)",
        "{",
        "    return 0;",
        "}",
    };
    FinderSymbol syms[32];
    /* lang=1 is LANG_C */
    int n = finder_extract_symbols(lines, 6, 1, syms, 32);
    ASSERT(n >= 1, "at least one symbol found");

    int found_main = 0;
    for (int i = 0; i < n; i++)
        if (strcmp(syms[i].name, "main") == 0) found_main = 1;
    ASSERT(found_main, "found main()");
}

TEST(test_symbols_c_struct_and_define)
{
    const char *lines[] = {
        "#define MAX_SIZE 100",
        "",
        "struct MyStruct {",
        "    int x;",
        "};",
    };
    FinderSymbol syms[32];
    int n = finder_extract_symbols(lines, 5, 1, syms, 32);
    ASSERT(n >= 2, "found define and struct");

    int found_define = 0, found_struct = 0;
    for (int i = 0; i < n; i++) {
        if (strcmp(syms[i].name, "MAX_SIZE") == 0 && syms[i].kind == 'd')
            found_define = 1;
        if (strcmp(syms[i].name, "MyStruct") == 0 && syms[i].kind == 's')
            found_struct = 1;
    }
    ASSERT(found_define, "found #define MAX_SIZE");
    ASSERT(found_struct, "found struct MyStruct");
}

TEST(test_symbols_python)
{
    const char *lines[] = {
        "import os",
        "",
        "class MyClass:",
        "    def __init__(self):",
        "        pass",
        "",
        "def helper():",
        "    pass",
    };
    FinderSymbol syms[32];
    /* lang=2 is LANG_PYTHON */
    int n = finder_extract_symbols(lines, 8, 2, syms, 32);

    int found_class = 0, found_def = 0;
    for (int i = 0; i < n; i++) {
        if (strcmp(syms[i].name, "MyClass") == 0) found_class = 1;
        if (strcmp(syms[i].name, "helper") == 0) found_def = 1;
    }
    ASSERT(found_class, "found class MyClass");
    ASSERT(found_def, "found def helper");
}

TEST(test_symbols_js_function)
{
    const char *lines[] = {
        "function greet(name) {",
        "  return 'hello ' + name;",
        "}",
        "",
        "class App {",
        "}",
    };
    FinderSymbol syms[32];
    /* lang=3 is LANG_JS */
    int n = finder_extract_symbols(lines, 6, 3, syms, 32);

    int found_fn = 0, found_class = 0;
    for (int i = 0; i < n; i++) {
        if (strcmp(syms[i].name, "greet") == 0) found_fn = 1;
        if (strcmp(syms[i].name, "App") == 0) found_class = 1;
    }
    ASSERT(found_fn, "found function greet");
    ASSERT(found_class, "found class App");
}

TEST(test_symbols_skips_control_flow)
{
    const char *lines[] = {
        "if (condition) {",
        "for (int i = 0; i < n; i++) {",
        "while (running) {",
        "return result;",
    };
    FinderSymbol syms[32];
    int n = finder_extract_symbols(lines, 4, 1, syms, 32);
    ASSERT(n == 0, "control flow lines not detected as symbols");
}

TEST(test_symbols_null_input)
{
    FinderSymbol syms[32];
    ASSERT(finder_extract_symbols(NULL, 0, 0, syms, 32) == 0,
           "NULL input returns 0");
}

TEST(test_symbols_enum)
{
    const char *lines[] = {
        "enum Color {",
        "    RED,",
        "    GREEN,",
        "    BLUE",
        "};",
    };
    FinderSymbol syms[32];
    int n = finder_extract_symbols(lines, 5, 1, syms, 32);
    ASSERT(n >= 1, "found enum");

    int found = 0;
    for (int i = 0; i < n; i++)
        if (strcmp(syms[i].name, "Color") == 0 && syms[i].kind == 'e')
            found = 1;
    ASSERT(found, "found enum Color");
}

/* ============================================================================
 * main
 * ============================================================================ */

int main(void)
{
    printf("=== test_finder ===\n");

    /* Fuzzy score */
    RUN(test_score_empty_query);
    RUN(test_score_exact_match);
    RUN(test_score_subsequence_match);
    RUN(test_score_no_match);
    RUN(test_score_case_insensitive);
    RUN(test_score_consecutive_bonus);
    RUN(test_score_separator_bonus);
    RUN(test_score_start_bonus);
    RUN(test_score_path_match);
    RUN(test_score_null_inputs);
    RUN(test_score_query_longer_than_text);

    /* Filter */
    RUN(test_filter_empty_query);
    RUN(test_filter_matching_query);
    RUN(test_filter_no_match);
    RUN(test_filter_sorted_by_score);
    RUN(test_filter_max_results);
    RUN(test_filter_null_inputs);

    /* File collection */
    RUN(test_collect_basic);
    RUN(test_collect_skips_hidden);
    RUN(test_collect_skips_obj_dir);

    /* Symbol extraction */
    RUN(test_symbols_c_function);
    RUN(test_symbols_c_struct_and_define);
    RUN(test_symbols_python);
    RUN(test_symbols_js_function);
    RUN(test_symbols_skips_control_flow);
    RUN(test_symbols_null_input);
    RUN(test_symbols_enum);

    TEST_SUMMARY();
}
