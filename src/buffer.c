/*
 * buffer.c — Text Buffer Implementation
 * =============================================================================
 * See buffer.h for the API documentation.
 * =============================================================================
 */

#include "buffer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

/* ============================================================================
 * Internal helpers
 * ============================================================================ */

/*
 * line_init — initialise a Line to an empty string.
 *
 * Allocates LINE_INITIAL_CAP bytes.  Returns 0 on success, -1 on failure.
 */
static int line_init(Line *line)
{
    line->text = malloc(LINE_INITIAL_CAP);
    if (!line->text) return -1;

    line->text[0] = '\0';
    line->len     = 0;
    line->cap     = LINE_INITIAL_CAP;
    return 0;
}

/*
 * line_free — free the heap memory owned by a Line.
 *
 * Does NOT free the Line struct itself (because Lines live inside arrays).
 */
static void line_free(Line *line)
{
    free(line->text);
    line->text = NULL;
    line->len  = 0;
    line->cap  = 0;
}

/*
 * line_ensure_cap — grow line->text so it can hold `needed` bytes (plus '\0').
 *
 * Uses a doubling strategy to keep reallocs infrequent.
 * Returns 0 on success, -1 on allocation failure.
 */
static int line_ensure_cap(Line *line, int needed)
{
    /* +1 for the null terminator */
    if (line->cap > needed) return 0;

    int new_cap = line->cap;
    while (new_cap <= needed)
        new_cap *= 2;

    char *p = realloc(line->text, new_cap);
    if (!p) return -1;

    line->text = p;
    line->cap  = new_cap;
    return 0;
}

/*
 * buffer_ensure_lines — grow the lines array so it can hold `needed` Line slots.
 *
 * Returns 0 on success, -1 on failure.
 */
static int buffer_ensure_lines(Buffer *buf, int needed)
{
    if (buf->capacity >= needed) return 0;

    int new_cap = buf->capacity;
    while (new_cap < needed)
        new_cap *= 2;

    Line *p = realloc(buf->lines, new_cap * sizeof(Line));
    if (!p) return -1;

    buf->lines    = p;
    buf->capacity = new_cap;
    return 0;
}

/* ============================================================================
 * Lifecycle
 * ============================================================================ */

Buffer *buffer_create(void)
{
    Buffer *buf = malloc(sizeof(Buffer));
    if (!buf) return NULL;

    buf->lines     = malloc(BUFFER_INITIAL_LINES * sizeof(Line));
    buf->num_lines = 0;
    buf->capacity  = BUFFER_INITIAL_LINES;
    buf->filename  = NULL;
    buf->dirty     = 0;

    if (!buf->lines) {
        free(buf);
        return NULL;
    }

    /* Every buffer starts with one empty line */
    if (line_init(&buf->lines[0]) != 0) {
        free(buf->lines);
        free(buf);
        return NULL;
    }
    buf->num_lines = 1;

    return buf;
}

void buffer_destroy(Buffer *buf)
{
    if (!buf) return;

    for (int i = 0; i < buf->num_lines; i++)
        line_free(&buf->lines[i]);

    free(buf->lines);
    free(buf->filename);
    free(buf);
}

/* ============================================================================
 * File I/O
 * ============================================================================ */

int buffer_load_file(Buffer *buf, const char *path)
{
    FILE *fp = fopen(path, "r");
    if (!fp) return -1;

    /* Free existing lines */
    for (int i = 0; i < buf->num_lines; i++)
        line_free(&buf->lines[i]);
    buf->num_lines = 0;

    /* Read the file line by line.
     * fgets reads up to (size-1) chars and includes the '\n' if present. */
    char read_buf[4096];
    while (fgets(read_buf, sizeof(read_buf), fp) != NULL) {
        /* Ensure space for one more line */
        if (buffer_ensure_lines(buf, buf->num_lines + 1) != 0) {
            fclose(fp);
            return -1;
        }

        Line *line = &buf->lines[buf->num_lines];
        if (line_init(line) != 0) {
            fclose(fp);
            return -1;
        }
        buf->num_lines++;

        /* Strip the trailing newline if present */
        int rlen = (int)strlen(read_buf);
        if (rlen > 0 && read_buf[rlen - 1] == '\n')
            rlen--;
        /* Also handle Windows-style \r\n */
        if (rlen > 0 && read_buf[rlen - 1] == '\r')
            rlen--;

        /* Copy text into the line */
        if (line_ensure_cap(line, rlen) != 0) {
            fclose(fp);
            return -1;
        }
        memcpy(line->text, read_buf, rlen);
        line->text[rlen] = '\0';
        line->len = rlen;
    }

    fclose(fp);

    /* If the file was completely empty, keep the single empty line */
    if (buf->num_lines == 0) {
        if (line_init(&buf->lines[0]) != 0) return -1;
        buf->num_lines = 1;
    }

    /* Store the filename */
    free(buf->filename);
    buf->filename = strdup(path);
    buf->dirty    = 0;

    return 0;
}

int buffer_save_file(Buffer *buf)
{
    if (!buf->filename) {
        errno = EINVAL;
        return -1;
    }
    return buffer_save_file_as(buf, buf->filename);
}

int buffer_save_file_as(Buffer *buf, const char *path)
{
    FILE *fp = fopen(path, "w");
    if (!fp) return -1;

    for (int i = 0; i < buf->num_lines; i++) {
        /* Write the line text */
        if (fwrite(buf->lines[i].text, 1, buf->lines[i].len, fp)
                != (size_t)buf->lines[i].len) {
            fclose(fp);
            return -1;
        }
        /* Write a newline after every line except possibly the last.
         * Most Unix tools expect a trailing newline, so we write it for all. */
        if (fputc('\n', fp) == EOF) {
            fclose(fp);
            return -1;
        }
    }

    fclose(fp);

    /* Update the stored filename if it changed */
    if (!buf->filename || strcmp(buf->filename, path) != 0) {
        free(buf->filename);
        buf->filename = strdup(path);
    }
    buf->dirty = 0;

    return 0;
}

/* ============================================================================
 * Text Mutation
 * ============================================================================ */

int buffer_insert_char(Buffer *buf, int row, int col, char c)
{
    /* Bounds checks */
    if (row < 0 || row >= buf->num_lines) return -1;

    Line *line = &buf->lines[row];

    if (col < 0 || col > line->len) return -1;

    /* Make sure there is room for one more character plus the null terminator */
    if (line_ensure_cap(line, line->len + 1) != 0) return -1;

    /* Shift everything from col onwards one position to the right */
    memmove(line->text + col + 1,
            line->text + col,
            line->len - col + 1);   /* +1 includes the '\0' */

    line->text[col] = c;
    line->len++;

    buf->dirty = 1;
    return 0;
}

int buffer_delete_char(Buffer *buf, int row, int col)
{
    if (row < 0 || row >= buf->num_lines) return -1;

    Line *line = &buf->lines[row];

    if (col < 0 || col >= line->len) return -1;

    /* Shift everything from col+1 onwards one position to the left.
     * The +1 in the count includes the null terminator. */
    memmove(line->text + col,
            line->text + col + 1,
            line->len - col);       /* includes '\0' */

    line->len--;
    buf->dirty = 1;
    return 0;
}

int buffer_insert_newline(Buffer *buf, int row, int col)
{
    if (row < 0 || row >= buf->num_lines) return -1;

    Line *current = &buf->lines[row];

    if (col < 0 || col > current->len) return -1;

    /* Make room for one more line in the array */
    if (buffer_ensure_lines(buf, buf->num_lines + 1) != 0) return -1;

    /* The text after `col` will move to the new line */
    int tail_len = current->len - col;

    /* Initialise a new line to hold the tail text */
    Line new_line;
    if (line_init(&new_line) != 0) return -1;

    if (tail_len > 0) {
        if (line_ensure_cap(&new_line, tail_len) != 0) {
            line_free(&new_line);
            return -1;
        }
        memcpy(new_line.text, current->text + col, tail_len);
        new_line.text[tail_len] = '\0';
        new_line.len = tail_len;
    }

    /* Truncate the current line at col */
    current->text[col] = '\0';
    current->len       = col;

    /* Shift all lines after `row` down by one slot */
    memmove(&buf->lines[row + 2],
            &buf->lines[row + 1],
            (buf->num_lines - row - 1) * sizeof(Line));

    /* Insert the new line at row+1 */
    buf->lines[row + 1] = new_line;
    buf->num_lines++;

    buf->dirty = 1;
    return 0;
}

int buffer_join_lines(Buffer *buf, int row)
{
    /* row+1 must exist */
    if (row < 0 || row >= buf->num_lines - 1) return -1;

    Line *line_a = &buf->lines[row];
    Line *line_b = &buf->lines[row + 1];

    /* Append line_b's text to line_a */
    int new_len = line_a->len + line_b->len;
    if (line_ensure_cap(line_a, new_len) != 0) return -1;

    memcpy(line_a->text + line_a->len, line_b->text, line_b->len + 1); /* +1 for '\0' */
    line_a->len = new_len;

    /* Free line_b's storage and shift lines up */
    line_free(line_b);

    memmove(&buf->lines[row + 1],
            &buf->lines[row + 2],
            (buf->num_lines - row - 2) * sizeof(Line));

    buf->num_lines--;
    buf->dirty = 1;
    return 0;
}

/* ============================================================================
 * Queries
 * ============================================================================ */

const char *buffer_get_line(const Buffer *buf, int row)
{
    if (row < 0 || row >= buf->num_lines) return NULL;
    return buf->lines[row].text;
}

int buffer_line_len(const Buffer *buf, int row)
{
    if (row < 0 || row >= buf->num_lines) return -1;
    return buf->lines[row].len;
}
