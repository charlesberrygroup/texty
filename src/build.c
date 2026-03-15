/*
 * build.c — Build System Integration Implementation
 * =============================================================================
 * Implements the functions declared in build.h.
 *
 * This module shells out to the build command using popen(), captures the
 * output, and parses gcc/clang-style error messages.  The parsed errors
 * are used by display.c to render the build panel and highlight error lines.
 *
 * The error parser handles the common gcc/clang output format:
 *   filepath:line:col: error: message
 *   filepath:line:col: warning: message
 *   filepath:line: error: message        (no column number)
 *   filepath:line:col: fatal error: message
 *
 * Other output lines (make commands, linker errors, notes, etc.) are
 * preserved in the raw output but not parsed into BuildError entries.
 * =============================================================================
 */

#include "build.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>    /* for isdigit() */

/* ============================================================================
 * Internal helper — run a shell command and capture its output
 * ============================================================================
 *
 * This is the same run_command() pattern used in git.c.  It's duplicated
 * here (as a static function) to keep build.c independent of git.c.
 * If more modules need it, it can be extracted to a shared util.c later.
 */

/*
 * run_build_command — run a shell command and read its stdout into a buffer.
 *
 * Returns a heap-allocated string (caller must free), or NULL on failure.
 * If `out_status` is non-NULL, the command's exit status is written there.
 * Trailing newlines are NOT stripped (we want the raw output for display).
 */
static char *run_build_command(const char *cmd, int *out_status)
{
    FILE *fp = popen(cmd, "r");
    if (!fp) return NULL;

    /*
     * Read all output into a growable buffer.
     * Build output can be large, so we start at 4 KB and double as needed.
     */
    int   cap = 4096;
    int   len = 0;
    char *buf = malloc(cap);
    if (!buf) { pclose(fp); return NULL; }

    int ch;
    while ((ch = fgetc(fp)) != EOF) {
        if (len + 1 >= cap) {
            cap *= 2;
            char *tmp = realloc(buf, cap);
            if (!tmp) { free(buf); pclose(fp); return NULL; }
            buf = tmp;
        }
        buf[len++] = (char)ch;
    }
    buf[len] = '\0';

    int status = pclose(fp);
    if (out_status)
        *out_status = status;

    return buf;
}

/* ============================================================================
 * Lifecycle
 * ============================================================================ */

void build_result_free(BuildResult *br)
{
    if (!br) return;
    free(br->output);
    br->output      = NULL;
    br->output_len  = 0;
    br->error_count = 0;
    br->exit_status = 0;
    br->ran         = 0;
}

/* ============================================================================
 * Error parsing
 * ============================================================================ */

/*
 * parse_one_line — attempt to parse a single line of compiler output.
 *
 * If the line matches the gcc/clang error format, populates `err` and
 * returns 1.  Otherwise returns 0 (not an error line — skip it).
 *
 * The format is:  filepath:line[:col]: (error|warning|fatal error): message
 *
 * We parse manually (not sscanf) because the filepath may contain colons
 * on some systems, and the format has variable structure.  The approach:
 *   1. Find the first ':' after position 0 (skip drive letters like C:)
 *   2. Check if a digit follows (line number)
 *   3. Parse line number, optionally column number
 *   4. Look for " error:" or " warning:" or " fatal error:"
 *   5. Extract the message after the final colon
 */
static int parse_one_line(const char *line, int line_len, BuildError *err)
{
    if (!line || line_len <= 0) return 0;

    /*
     * Skip common non-error prefixes.
     * Lines starting with these are never compiler diagnostics.
     */
    if (strncmp(line, "make", 4) == 0 && line[4] == '[') return 0;
    if (strncmp(line, "In file", 7) == 0) return 0;

    /*
     * Find the first colon after position 0.
     * We start at position 1 to skip potential drive letters (C:\...)
     * on Windows, though texty targets macOS/Linux.
     */
    const char *end = line + line_len;
    const char *colon1 = memchr(line + 1, ':', end - (line + 1));
    if (!colon1 || colon1 + 1 >= end) return 0;

    /* The character after the colon must be a digit (the line number) */
    if (!isdigit((unsigned char)colon1[1])) return 0;

    /* Extract filepath (everything before the first colon) */
    int path_len = (int)(colon1 - line);
    if (path_len <= 0 || path_len >= (int)sizeof(err->filepath)) return 0;
    memcpy(err->filepath, line, path_len);
    err->filepath[path_len] = '\0';

    /* Parse line number */
    err->line = 0;
    const char *p = colon1 + 1;
    while (p < end && isdigit((unsigned char)*p)) {
        err->line = err->line * 10 + (*p - '0');
        p++;
    }
    if (err->line <= 0) return 0;

    /* Check for column number (another colon followed by digits) */
    err->col = 0;
    if (p < end && *p == ':' && p + 1 < end && isdigit((unsigned char)p[1])) {
        p++;  /* skip colon */
        while (p < end && isdigit((unsigned char)*p)) {
            err->col = err->col * 10 + (*p - '0');
            p++;
        }
    }

    /* Expect ": " after the numbers */
    if (p >= end || *p != ':') return 0;
    p++;  /* skip colon */
    while (p < end && *p == ' ') p++;

    /* Check for "error", "warning", or "fatal error" */
    if (p + 5 <= end && strncmp(p, "error", 5) == 0) {
        err->is_warning = 0;
        p += 5;
    } else if (p + 7 <= end && strncmp(p, "warning", 7) == 0) {
        err->is_warning = 1;
        p += 7;
    } else if (p + 11 <= end && strncmp(p, "fatal error", 11) == 0) {
        err->is_warning = 0;
        p += 11;
    } else {
        return 0;  /* not an error or warning line */
    }

    /* Skip past the colon after "error"/"warning" */
    if (p < end && *p == ':') p++;
    while (p < end && *p == ' ') p++;

    /* Extract the message (rest of the line) */
    int msg_len = (int)(end - p);
    if (msg_len >= (int)sizeof(err->message))
        msg_len = (int)sizeof(err->message) - 1;
    if (msg_len > 0)
        memcpy(err->message, p, msg_len);
    err->message[msg_len] = '\0';

    /* Strip trailing newline/carriage return from message */
    while (msg_len > 0 && (err->message[msg_len - 1] == '\n'
                           || err->message[msg_len - 1] == '\r'))
        err->message[--msg_len] = '\0';

    return 1;
}

int build_parse_errors(BuildResult *br, const char *output)
{
    if (!br) return 0;
    br->error_count = 0;

    if (!output || output[0] == '\0') return 0;

    /*
     * Walk through the output line by line.
     * For each line, attempt to parse it as a compiler diagnostic.
     */
    const char *p = output;
    while (*p && br->error_count < BUILD_MAX_ERRORS) {
        /* Find end of current line */
        const char *eol = p;
        while (*eol && *eol != '\n') eol++;
        int line_len = (int)(eol - p);

        if (line_len > 0) {
            BuildError *err = &br->errors[br->error_count];
            if (parse_one_line(p, line_len, err))
                br->error_count++;
        }

        /* Advance past the newline */
        p = eol;
        if (*p == '\n') p++;
    }

    return br->error_count;
}

int build_error_matches_buffer(const BuildError *err, const char *buf_filename)
{
    if (!err || !buf_filename || buf_filename[0] == '\0')
        return 0;
    if (err->filepath[0] == '\0')
        return 0;

    /*
     * Try exact match first (both are absolute paths).
     */
    if (strcmp(err->filepath, buf_filename) == 0)
        return 1;

    /*
     * Try suffix match: check if the buffer's filename ends with the
     * error's filepath.  This handles the common case where the compiler
     * outputs "src/main.c" and the buffer has "/home/user/project/src/main.c".
     *
     * We require the character before the suffix to be '/' to avoid
     * false matches like "xmain.c" matching "main.c".
     */
    int err_len = (int)strlen(err->filepath);
    int buf_len = (int)strlen(buf_filename);

    if (err_len > buf_len) return 0;
    if (err_len == buf_len)
        return strcmp(err->filepath, buf_filename) == 0;

    /* Check if buf_filename ends with /err->filepath */
    const char *suffix = buf_filename + (buf_len - err_len);
    if (suffix[-1] == '/' && strcmp(suffix, err->filepath) == 0)
        return 1;

    return 0;
}

/* ============================================================================
 * Build execution
 * ============================================================================ */

int build_run(BuildResult *br, const char *command, const char *working_dir)
{
    if (!br || !command || command[0] == '\0') return -1;

    /* Clear previous results */
    build_result_free(br);

    /*
     * Build the shell command.
     *
     * We cd to the working directory first, then run the build command.
     * The 2>&1 redirects stderr to stdout so we capture all output
     * (compiler errors go to stderr, make output goes to stdout).
     *
     * The semicolon before "echo" ensures we always get the exit status
     * even if the build command fails.
     */
    char cmd[4096];
    if (working_dir && working_dir[0] != '\0')
        snprintf(cmd, sizeof(cmd),
                 "cd '%s' && %s 2>&1", working_dir, command);
    else
        snprintf(cmd, sizeof(cmd), "%s 2>&1", command);

    int exit_status = 0;
    char *output = run_build_command(cmd, &exit_status);
    if (!output) return -1;

    br->output      = output;
    br->output_len  = (int)strlen(output);
    br->exit_status = exit_status;
    br->ran         = 1;

    /* Parse errors from the output */
    build_parse_errors(br, output);

    return 0;
}

/* ============================================================================
 * Configuration — texty.json
 * ============================================================================ */

void build_parse_config(char *out_command, int out_size, const char *json_text)
{
    /* Default to "make" */
    strncpy(out_command, "make", out_size - 1);
    out_command[out_size - 1] = '\0';

    if (!json_text || json_text[0] == '\0') return;

    /*
     * Simple manual JSON parsing — we only need the "build_command" key.
     *
     * Strategy: find the string "build_command", skip to the ':', skip
     * whitespace, read the quoted string value.  No need for a full JSON
     * parser since we only support one key.
     */
    const char *key = strstr(json_text, "\"build_command\"");
    if (!key) return;

    /* Skip past the key and find the colon */
    const char *p = key + 15;  /* strlen("\"build_command\"") = 15 */
    while (*p && *p != ':') p++;
    if (!*p) return;
    p++;  /* skip colon */

    /* Skip whitespace */
    while (*p && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;

    /* Expect opening quote */
    if (*p != '"') return;
    p++;  /* skip opening quote */

    /* Read until closing quote */
    const char *start = p;
    while (*p && *p != '"') {
        /* Handle escaped quotes */
        if (*p == '\\' && p[1] == '"') p++;
        p++;
    }

    int value_len = (int)(p - start);
    if (value_len <= 0) return;
    if (value_len >= out_size) value_len = out_size - 1;

    memcpy(out_command, start, value_len);
    out_command[value_len] = '\0';
}

void build_load_config(char *out_command, int out_size,
                       const char *project_dir)
{
    /* Default to "make" */
    strncpy(out_command, "make", out_size - 1);
    out_command[out_size - 1] = '\0';

    if (!project_dir || project_dir[0] == '\0') return;

    /* Try to open texty.json in the project directory */
    char path[2048];
    snprintf(path, sizeof(path), "%s/texty.json", project_dir);

    FILE *fp = fopen(path, "r");
    if (!fp) return;  /* no config file — keep default "make" */

    /*
     * Read the entire file into a buffer.
     * texty.json should be small (< 4 KB), so we read it all at once.
     */
    fseek(fp, 0, SEEK_END);
    long file_size = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    if (file_size <= 0 || file_size > 4096) {
        fclose(fp);
        return;
    }

    char *json = malloc(file_size + 1);
    if (!json) { fclose(fp); return; }

    int read = (int)fread(json, 1, file_size, fp);
    fclose(fp);
    json[read] = '\0';

    build_parse_config(out_command, out_size, json);
    free(json);
}
