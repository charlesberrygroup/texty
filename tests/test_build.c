/*
 * test_build.c — Unit tests for build.c
 * =============================================================================
 * Tests the build error parser and config parser without running actual
 * build commands.  The key function under test is build_parse_errors(),
 * which takes raw compiler output and populates BuildError entries.
 * =============================================================================
 */

#include "test_runner.h"
#include "build.h"

#include <stdlib.h>
#include <string.h>

/* ============================================================================
 * Lifecycle
 * ============================================================================ */

TEST(test_build_result_free_zeroed)
{
    BuildResult br;
    memset(&br, 0, sizeof(br));
    build_result_free(&br);
    ASSERT(br.output == NULL, "output NULL after free");
    ASSERT(br.error_count == 0, "error_count 0 after free");

    /* Double free should be safe */
    build_result_free(&br);
    ASSERT(1, "double free did not crash");
}

/* ============================================================================
 * Error parsing — basic cases
 * ============================================================================ */

TEST(test_parse_empty_output)
{
    BuildResult br;
    memset(&br, 0, sizeof(br));

    int count = build_parse_errors(&br, "");
    ASSERT(count == 0, "empty output → 0 errors");
    ASSERT(br.error_count == 0, "error_count is 0");
}

TEST(test_parse_null_output)
{
    BuildResult br;
    memset(&br, 0, sizeof(br));

    int count = build_parse_errors(&br, NULL);
    ASSERT(count == 0, "NULL output → 0 errors");
}

TEST(test_parse_single_error)
{
    BuildResult br;
    memset(&br, 0, sizeof(br));

    const char *output = "main.c:10:5: error: expected ';' after expression\n";
    int count = build_parse_errors(&br, output);

    ASSERT(count == 1, "one error parsed");
    ASSERT(strcmp(br.errors[0].filepath, "main.c") == 0, "filepath");
    ASSERT(br.errors[0].line == 10, "line");
    ASSERT(br.errors[0].col == 5, "col");
    ASSERT(br.errors[0].is_warning == 0, "is error not warning");
    ASSERT(strstr(br.errors[0].message, "expected ';'") != NULL, "message");
}

TEST(test_parse_single_warning)
{
    BuildResult br;
    memset(&br, 0, sizeof(br));

    const char *output =
        "main.c:3:1: warning: unused variable 'x' [-Wunused-variable]\n";
    int count = build_parse_errors(&br, output);

    ASSERT(count == 1, "one warning parsed");
    ASSERT(br.errors[0].is_warning == 1, "is warning");
    ASSERT(br.errors[0].line == 3, "line");
    ASSERT(br.errors[0].col == 1, "col");
}

TEST(test_parse_error_no_column)
{
    BuildResult br;
    memset(&br, 0, sizeof(br));

    /* Older gcc format without column number */
    const char *output = "util.c:42: error: undeclared identifier\n";
    int count = build_parse_errors(&br, output);

    ASSERT(count == 1, "one error parsed");
    ASSERT(strcmp(br.errors[0].filepath, "util.c") == 0, "filepath");
    ASSERT(br.errors[0].line == 42, "line");
    ASSERT(br.errors[0].col == 0, "col is 0 when absent");
    ASSERT(br.errors[0].is_warning == 0, "is error");
}

TEST(test_parse_fatal_error)
{
    BuildResult br;
    memset(&br, 0, sizeof(br));

    const char *output =
        "main.c:1:10: fatal error: missing.h: No such file or directory\n";
    int count = build_parse_errors(&br, output);

    ASSERT(count == 1, "one fatal error parsed");
    ASSERT(br.errors[0].is_warning == 0, "fatal error is not a warning");
    ASSERT(br.errors[0].line == 1, "line");
}

TEST(test_parse_multiple_errors)
{
    BuildResult br;
    memset(&br, 0, sizeof(br));

    const char *output =
        "main.c:10:5: error: expected ';'\n"
        "main.c:15:1: warning: unused variable\n"
        "util.c:3:8: error: undeclared identifier\n";
    int count = build_parse_errors(&br, output);

    ASSERT(count == 3, "three diagnostics parsed");
    ASSERT(br.errors[0].is_warning == 0, "first is error");
    ASSERT(br.errors[1].is_warning == 1, "second is warning");
    ASSERT(br.errors[2].is_warning == 0, "third is error");
    ASSERT(strcmp(br.errors[2].filepath, "util.c") == 0, "third filepath");
}

TEST(test_parse_mixed_output)
{
    /*
     * Real-world gcc output with make commands, "In file included from"
     * lines, and actual errors.  Only errors/warnings should be parsed.
     */
    BuildResult br;
    memset(&br, 0, sizeof(br));

    const char *output =
        "gcc -Wall -c -o main.o main.c\n"
        "In file included from main.c:3:\n"
        "header.h:5:1: error: unknown type name 'foo'\n"
        "make[1]: Entering directory '/home/user/project'\n"
        "main.c:20:3: warning: implicit declaration of function 'bar'\n"
        "gcc -o program main.o\n"
        "/usr/bin/ld: undefined reference to 'bar'\n"
        "collect2: error: ld returned 1 exit status\n"
        "make: *** [Makefile:10: program] Error 1\n";

    int count = build_parse_errors(&br, output);

    /* Only the file:line:col errors/warnings should be parsed */
    ASSERT(count == 2, "two diagnostics from mixed output");
    ASSERT(strcmp(br.errors[0].filepath, "header.h") == 0,
           "first error from header.h");
    ASSERT(br.errors[0].line == 5, "first error line");
    ASSERT(strcmp(br.errors[1].filepath, "main.c") == 0,
           "second error from main.c");
    ASSERT(br.errors[1].is_warning == 1, "second is warning");
}

TEST(test_parse_path_with_directory)
{
    BuildResult br;
    memset(&br, 0, sizeof(br));

    const char *output = "src/editor.c:100:3: error: bad code\n";
    build_parse_errors(&br, output);

    ASSERT(br.error_count == 1, "one error");
    ASSERT(strcmp(br.errors[0].filepath, "src/editor.c") == 0,
           "filepath includes directory");
}

TEST(test_parse_linker_error_skipped)
{
    BuildResult br;
    memset(&br, 0, sizeof(br));

    const char *output =
        "/usr/bin/ld: undefined reference to 'main'\n"
        "collect2: error: ld returned 1 exit status\n";
    int count = build_parse_errors(&br, output);

    /* Linker errors don't match file:line:col format */
    ASSERT(count == 0, "linker errors not parsed as BuildError");
}

TEST(test_parse_message_truncation)
{
    BuildResult br;
    memset(&br, 0, sizeof(br));

    /* Build a line with a very long message */
    char line[2048];
    strcpy(line, "f.c:1:1: error: ");
    for (int i = (int)strlen(line); i < 2040; i++)
        line[i] = 'x';
    line[2040] = '\n';
    line[2041] = '\0';

    build_parse_errors(&br, line);
    ASSERT(br.error_count == 1, "error parsed despite long message");
    ASSERT((int)strlen(br.errors[0].message) < 512,
           "message truncated to fit buffer");
}

/* ============================================================================
 * Filename matching
 * ============================================================================ */

TEST(test_match_exact_path)
{
    BuildError err;
    strcpy(err.filepath, "/home/user/project/main.c");

    ASSERT(build_error_matches_buffer(&err, "/home/user/project/main.c") == 1,
           "exact path matches");
}

TEST(test_match_relative_suffix)
{
    BuildError err;
    strcpy(err.filepath, "src/main.c");

    ASSERT(build_error_matches_buffer(&err,
               "/home/user/project/src/main.c") == 1,
           "relative path matches as suffix");
}

TEST(test_match_basename_only)
{
    BuildError err;
    strcpy(err.filepath, "main.c");

    ASSERT(build_error_matches_buffer(&err,
               "/home/user/project/main.c") == 1,
           "basename matches");
}

TEST(test_match_no_match)
{
    BuildError err;
    strcpy(err.filepath, "other.c");

    ASSERT(build_error_matches_buffer(&err, "/home/user/project/main.c") == 0,
           "different file does not match");
}

TEST(test_match_partial_name_no_match)
{
    BuildError err;
    strcpy(err.filepath, "main.c");

    /* "xmain.c" should NOT match "main.c" — the character before must be '/' */
    ASSERT(build_error_matches_buffer(&err, "/project/xmain.c") == 0,
           "partial name does not match");
}

TEST(test_match_null_inputs)
{
    BuildError err;
    strcpy(err.filepath, "main.c");

    ASSERT(build_error_matches_buffer(&err, NULL) == 0,
           "NULL buffer filename");
    ASSERT(build_error_matches_buffer(NULL, "main.c") == 0,
           "NULL error");
}

/* ============================================================================
 * Config parsing
 * ============================================================================ */

TEST(test_config_parse_valid)
{
    char cmd[BUILD_CMD_MAX];
    build_parse_config(cmd, sizeof(cmd),
                       "{ \"build_command\": \"make -j4\" }");
    ASSERT(strcmp(cmd, "make -j4") == 0, "parsed build command");
}

TEST(test_config_parse_default)
{
    char cmd[BUILD_CMD_MAX];
    build_parse_config(cmd, sizeof(cmd), "{}");
    ASSERT(strcmp(cmd, "make") == 0, "default is make");
}

TEST(test_config_parse_null)
{
    char cmd[BUILD_CMD_MAX];
    build_parse_config(cmd, sizeof(cmd), NULL);
    ASSERT(strcmp(cmd, "make") == 0, "NULL input defaults to make");
}

TEST(test_config_parse_empty)
{
    char cmd[BUILD_CMD_MAX];
    build_parse_config(cmd, sizeof(cmd), "");
    ASSERT(strcmp(cmd, "make") == 0, "empty input defaults to make");
}

TEST(test_config_parse_no_key)
{
    char cmd[BUILD_CMD_MAX];
    build_parse_config(cmd, sizeof(cmd),
                       "{ \"other_key\": \"value\" }");
    ASSERT(strcmp(cmd, "make") == 0, "missing key defaults to make");
}

TEST(test_config_parse_complex_command)
{
    char cmd[BUILD_CMD_MAX];
    build_parse_config(cmd, sizeof(cmd),
                       "{ \"build_command\": \"cmake --build build -- -j8\" }");
    ASSERT(strcmp(cmd, "cmake --build build -- -j8") == 0,
           "complex command parsed correctly");
}

/* ============================================================================
 * main
 * ============================================================================ */

int main(void)
{
    printf("=== test_build ===\n");

    /* Lifecycle */
    RUN(test_build_result_free_zeroed);

    /* Error parsing */
    RUN(test_parse_empty_output);
    RUN(test_parse_null_output);
    RUN(test_parse_single_error);
    RUN(test_parse_single_warning);
    RUN(test_parse_error_no_column);
    RUN(test_parse_fatal_error);
    RUN(test_parse_multiple_errors);
    RUN(test_parse_mixed_output);
    RUN(test_parse_path_with_directory);
    RUN(test_parse_linker_error_skipped);
    RUN(test_parse_message_truncation);

    /* Filename matching */
    RUN(test_match_exact_path);
    RUN(test_match_relative_suffix);
    RUN(test_match_basename_only);
    RUN(test_match_no_match);
    RUN(test_match_partial_name_no_match);
    RUN(test_match_null_inputs);

    /* Config parsing */
    RUN(test_config_parse_valid);
    RUN(test_config_parse_default);
    RUN(test_config_parse_null);
    RUN(test_config_parse_empty);
    RUN(test_config_parse_no_key);
    RUN(test_config_parse_complex_command);

    TEST_SUMMARY();
}
