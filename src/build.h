/*
 * build.h — Build System Integration (pure logic, no ncurses)
 * =============================================================================
 * This module handles running build commands, parsing compiler error output,
 * and loading project configuration from texty.json.
 *
 * Architecture
 * ------------
 * All build operations shell out to the build command (e.g. "make") using
 * popen().  The raw output is captured and then parsed line-by-line to
 * extract file:line:col error/warning messages in gcc/clang format.
 *
 * The parsed errors are stored in a BuildResult struct which the display
 * module reads to render the build output panel and highlight error lines
 * in the gutter.
 *
 * This module has NO knowledge of ncurses or the display — it only produces
 * data that display.c reads when rendering.
 * =============================================================================
 */

#ifndef BUILD_H
#define BUILD_H

/* ---- Constants ------------------------------------------------------------ */

/** Maximum number of parsed errors/warnings from a single build. */
#define BUILD_MAX_ERRORS  256

/** Maximum length of the build command string. */
#define BUILD_CMD_MAX     512

/* ---- Data Types ----------------------------------------------------------- */

/*
 * BuildError — a single parsed compiler diagnostic.
 *
 * Extracted from lines matching the gcc/clang format:
 *   file.c:42:10: error: expected ';' after expression
 *   file.c:15:1: warning: unused variable 'x'
 *
 * The filepath is stored as-is from the compiler output (may be relative
 * to the build directory).  The line and col are 1-based, matching the
 * compiler's numbering.
 */
typedef struct {
    char filepath[1024];   /* path from the compiler output              */
    int  line;             /* 1-based line number                        */
    int  col;              /* 1-based column (0 if not present)          */
    int  is_warning;       /* 1 = warning, 0 = error                    */
    char message[512];     /* the diagnostic message text                */
} BuildError;

/*
 * BuildResult — output and parsed errors from the last build.
 *
 * Contains both the raw output (for display in the build panel) and
 * a structured array of parsed errors/warnings (for jump-to-error and
 * gutter highlighting).
 *
 * Lifecycle: allocated by editor_build(), freed by build_result_free().
 */
typedef struct {
    /* Raw build output — heap-allocated, for display in the panel */
    char  *output;         /* full captured stdout+stderr               */
    int    output_len;     /* length of output string                   */

    /* Parsed errors/warnings — extracted from the output */
    BuildError errors[BUILD_MAX_ERRORS];
    int        error_count;

    /* Build metadata */
    int    exit_status;    /* 0 = success, non-zero = failure            */
    int    ran;            /* 1 if a build has been run at least once    */
} BuildResult;

/* ---- Lifecycle ------------------------------------------------------------ */

/**
 * build_result_free — free heap memory in a BuildResult.
 *
 * Frees the output string and resets all fields to zero.
 * Safe to call on an already-freed or zeroed BuildResult.
 */
void build_result_free(BuildResult *br);

/* ---- Error parsing -------------------------------------------------------- */

/**
 * build_parse_errors — parse gcc/clang error output into structured entries.
 *
 * Scans `output` line by line for lines matching the format:
 *   filepath:line:col: error: message
 *   filepath:line:col: warning: message
 *   filepath:line: error: message        (no column)
 *   filepath:line:col: fatal error: message
 *
 * Other lines (make output, linker errors, notes) are skipped.
 * Populates br->errors[] up to BUILD_MAX_ERRORS entries.
 *
 * Returns the number of errors/warnings parsed.
 * Exposed publicly so it can be unit-tested with known input strings.
 */
int build_parse_errors(BuildResult *br, const char *output);

/**
 * build_error_matches_buffer — check if an error's filepath matches a buffer.
 *
 * Compares the error's filepath against the buffer's filename using
 * suffix matching.  For example, error path "src/main.c" matches buffer
 * "/home/user/project/src/main.c".
 *
 * `buf_filename` is the buffer's filename (absolute path, or NULL).
 * Returns 1 if the error matches, 0 otherwise.
 */
int build_error_matches_buffer(const BuildError *err, const char *buf_filename);

/* ---- Build execution ------------------------------------------------------ */

/**
 * build_run — run a build command and capture its output.
 *
 * Executes `command` in the given `working_dir` using popen().
 * Both stdout and stderr are captured (via 2>&1 redirection).
 * Populates br with the raw output and parsed errors.
 *
 * Returns 0 on success (build ran, regardless of whether it passed),
 * -1 if the command could not be executed.
 */
int build_run(BuildResult *br, const char *command, const char *working_dir);

/* ---- Configuration -------------------------------------------------------- */

/**
 * build_load_config — load build command from texty.json.
 *
 * Searches for texty.json in `project_dir` and reads the
 * "build_command" key.  If the file doesn't exist or the key is
 * missing, `out_command` is set to "make" (the default).
 *
 * `out_command` must be at least BUILD_CMD_MAX bytes.
 */
void build_load_config(char *out_command, int out_size,
                       const char *project_dir);

/**
 * build_parse_config — parse a texty.json string for the build command.
 *
 * Pure parsing function — no file I/O.  Exposed publicly for testing.
 * If the key is not found, `out_command` is set to "make".
 */
void build_parse_config(char *out_command, int out_size, const char *json_text);

#endif /* BUILD_H */
