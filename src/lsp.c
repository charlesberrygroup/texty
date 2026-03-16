/*
 * lsp.c — Language Server Protocol Client Implementation
 * =============================================================================
 * Implements the functions declared in lsp.h.
 *
 * This module handles:
 *   - Message framing (Content-Length parsing and building)
 *   - Process management (fork/exec with pipes)
 *   - Sending requests and notifications
 *   - Non-blocking reading of responses
 *   - URI ↔ path conversion
 * =============================================================================
 */

#include "lsp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>      /* read, write, close, pipe, fork, exec */
#include <signal.h>       /* kill, SIGTERM */
#include <fcntl.h>        /* fcntl, O_NONBLOCK */
#include <sys/wait.h>     /* waitpid */
#include <errno.h>

/* ============================================================================
 * Message framing
 * ============================================================================ */

void lsp_framer_init(LspFramer *f)
{
    memset(f, 0, sizeof(LspFramer));
}

void lsp_framer_free(LspFramer *f)
{
    if (!f) return;
    free(f->buf);
    f->buf = NULL;
    f->len = 0;
    f->cap = 0;
}

int lsp_framer_feed(LspFramer *f, const char *data, int data_len)
{
    if (!f || !data || data_len <= 0) return 0;

    /* Grow the buffer if needed */
    while (f->len + data_len >= f->cap) {
        int new_cap = f->cap == 0 ? 4096 : f->cap * 2;
        if (new_cap > LSP_MAX_MSG_SIZE) return -1;
        char *tmp = realloc(f->buf, new_cap);
        if (!tmp) return -1;
        f->buf = tmp;
        f->cap = new_cap;
    }

    memcpy(f->buf + f->len, data, data_len);
    f->len += data_len;
    return 0;
}

char *lsp_framer_next(LspFramer *f)
{
    if (!f || !f->buf || f->len == 0) return NULL;

    /*
     * Scan for the header/body separator: "\r\n\r\n".
     * The header contains "Content-Length: N" (and possibly other headers).
     */
    char *sep = NULL;
    for (int i = 0; i + 3 < f->len; i++) {
        if (f->buf[i] == '\r' && f->buf[i+1] == '\n'
                && f->buf[i+2] == '\r' && f->buf[i+3] == '\n') {
            sep = f->buf + i;
            break;
        }
    }
    if (!sep) return NULL;  /* no complete header yet */

    /* Parse Content-Length from the header */
    int content_length = -1;
    const char *cl = strstr(f->buf, "Content-Length:");
    if (!cl || cl >= sep) {
        /* No Content-Length header found — malformed, skip this header */
        int skip = (int)(sep - f->buf) + 4;
        memmove(f->buf, f->buf + skip, f->len - skip);
        f->len -= skip;
        return NULL;
    }
    content_length = atoi(cl + 15);  /* skip "Content-Length:" */
    if (content_length <= 0 || content_length > LSP_MAX_MSG_SIZE) {
        /* Invalid length — skip */
        int skip = (int)(sep - f->buf) + 4;
        memmove(f->buf, f->buf + skip, f->len - skip);
        f->len -= skip;
        return NULL;
    }

    /* Check if we have the full body */
    int header_end = (int)(sep - f->buf) + 4;  /* after \r\n\r\n */
    int total = header_end + content_length;
    if (f->len < total) return NULL;  /* body not fully received yet */

    /* Extract the JSON body */
    char *body = malloc(content_length + 1);
    if (!body) return NULL;
    memcpy(body, f->buf + header_end, content_length);
    body[content_length] = '\0';

    /* Remove the consumed message from the buffer */
    memmove(f->buf, f->buf + total, f->len - total);
    f->len -= total;

    return body;
}

char *lsp_frame_message(const char *json_body)
{
    if (!json_body) return NULL;

    int body_len = (int)strlen(json_body);
    /*
     * Header format: "Content-Length: <N>\r\n\r\n"
     * Maximum header size: ~30 bytes (Content-Length: 16777216\r\n\r\n)
     */
    int total = 30 + body_len + 1;
    char *msg = malloc(total);
    if (!msg) return NULL;

    int hdr_len = snprintf(msg, total, "Content-Length: %d\r\n\r\n", body_len);
    memcpy(msg + hdr_len, json_body, body_len);
    msg[hdr_len + body_len] = '\0';

    return msg;
}

/* ============================================================================
 * URI helpers
 * ============================================================================ */

int lsp_path_to_uri(const char *path, char *out, int out_size)
{
    if (!path || !out || out_size <= 0) return -1;
    /*
     * Simple conversion: prepend "file://" to the absolute path.
     * A full implementation would percent-encode special characters,
     * but for typical source file paths this is sufficient.
     */
    snprintf(out, out_size, "file://%s", path);
    return 0;
}

int lsp_uri_to_path(const char *uri, char *out, int out_size)
{
    if (!uri || !out || out_size <= 0) return -1;

    /* Strip "file://" prefix */
    if (strncmp(uri, "file://", 7) == 0) {
        strncpy(out, uri + 7, out_size - 1);
        out[out_size - 1] = '\0';
        return 0;
    }

    /* Not a file URI — copy as-is */
    strncpy(out, uri, out_size - 1);
    out[out_size - 1] = '\0';
    return 0;
}

/* ============================================================================
 * Diagnostic parsing
 * ============================================================================ */

int lsp_parse_diagnostics(LspDiagnosticList *out, const JsonValue *params)
{
    if (!out || !params) return 0;
    out->count = 0;

    /*
     * The params object has:
     *   uri: string
     *   diagnostics: array of diagnostic objects
     *
     * Each diagnostic has:
     *   range: { start: {line, character}, end: {line, character} }
     *   severity: number (1=error, 2=warning, 3=info, 4=hint)
     *   message: string
     */
    JsonValue *diag_array = json_get(params, "diagnostics");
    if (!diag_array) return 0;

    int count = json_array_len(diag_array);
    if (count > LSP_MAX_DIAGNOSTICS)
        count = LSP_MAX_DIAGNOSTICS;

    for (int i = 0; i < count; i++) {
        JsonValue *d = json_array_get(diag_array, i);
        if (!d) continue;

        LspDiagnostic *diag = &out->items[out->count];

        /* Parse range */
        JsonValue *range = json_get(d, "range");
        if (range) {
            JsonValue *start = json_get(range, "start");
            JsonValue *end   = json_get(range, "end");
            diag->line     = json_get_int(json_get(start, "line"), 0);
            diag->col      = json_get_int(json_get(start, "character"), 0);
            diag->end_line = json_get_int(json_get(end, "line"), 0);
            diag->end_col  = json_get_int(json_get(end, "character"), 0);
        }

        /* Parse severity (default to warning if missing) */
        diag->severity = json_get_int(json_get(d, "severity"), LSP_SEV_WARNING);

        /* Parse message */
        const char *msg = json_get_string(json_get(d, "message"));
        if (msg) {
            strncpy(diag->message, msg, sizeof(diag->message) - 1);
            diag->message[sizeof(diag->message) - 1] = '\0';
        } else {
            diag->message[0] = '\0';
        }

        out->count++;
    }

    return out->count;
}

/* ============================================================================
 * Completion parsing
 * ============================================================================ */

int lsp_parse_completions(LspCompletionItem *out, int max_items,
                          const JsonValue *result)
{
    if (!out || !result || max_items <= 0) return 0;

    /*
     * The completion result can be either:
     *   1. An array of CompletionItem objects
     *   2. A CompletionList object with an "items" array
     */
    const JsonValue *items = result;
    if (result->type == JSON_OBJECT) {
        items = json_get(result, "items");
        if (!items) return 0;
    }
    if (items->type != JSON_ARRAY) return 0;

    int count = json_array_len(items);
    if (count > max_items) count = max_items;

    int parsed = 0;
    for (int i = 0; i < count; i++) {
        JsonValue *item = json_array_get(items, i);
        if (!item) continue;

        LspCompletionItem *ci = &out[parsed];
        memset(ci, 0, sizeof(LspCompletionItem));

        /* label (required) */
        const char *label = json_get_string(json_get(item, "label"));
        if (!label || label[0] == '\0') continue;
        strncpy(ci->label, label, sizeof(ci->label) - 1);

        /* insertText (optional — falls back to label) */
        const char *insert = json_get_string(json_get(item, "insertText"));
        if (insert && insert[0])
            strncpy(ci->insert_text, insert, sizeof(ci->insert_text) - 1);
        else
            strncpy(ci->insert_text, label, sizeof(ci->insert_text) - 1);

        /* detail (optional — type info) */
        const char *detail = json_get_string(json_get(item, "detail"));
        if (detail)
            strncpy(ci->detail, detail, sizeof(ci->detail) - 1);

        /* kind (optional) */
        ci->kind = json_get_int(json_get(item, "kind"), 0);

        parsed++;
    }

    return parsed;
}

/* ============================================================================
 * Process management
 * ============================================================================ */

LspServer *lsp_server_start(const char *command, const char *root_path)
{
    if (!command || !root_path) return NULL;

    /*
     * Create two pipes:
     *   to_server:   parent writes → child reads (child's stdin)
     *   from_server: child writes → parent reads (child's stdout)
     *
     * pipe() creates a pair of file descriptors:
     *   fd[0] = read end,  fd[1] = write end
     */
    int to_child[2], from_child[2];
    if (pipe(to_child) != 0 || pipe(from_child) != 0)
        return NULL;

    pid_t pid = fork();
    if (pid < 0) {
        /* Fork failed */
        close(to_child[0]); close(to_child[1]);
        close(from_child[0]); close(from_child[1]);
        return NULL;
    }

    if (pid == 0) {
        /*
         * Child process — becomes the language server.
         *
         * Redirect stdin to read from to_child[0] and stdout to
         * write to from_child[1].  Close unused pipe ends.
         */
        close(to_child[1]);    /* child doesn't write to its own stdin pipe */
        close(from_child[0]);  /* child doesn't read from its own stdout pipe */

        dup2(to_child[0], STDIN_FILENO);
        dup2(from_child[1], STDOUT_FILENO);

        /*
         * Redirect stderr to /dev/null.
         *
         * Language servers (especially clangd) write log messages to stderr.
         * If stderr is not redirected, those messages go to the terminal
         * and corrupt the ncurses display.
         */
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }

        close(to_child[0]);
        close(from_child[1]);

        /* Execute the language server command via the shell */
        execlp("sh", "sh", "-c", command, (char *)NULL);

        /* If exec fails, exit the child */
        _exit(1);
    }

    /*
     * Parent process — set up the LspServer struct.
     */
    close(to_child[0]);    /* parent doesn't read from child's stdin */
    close(from_child[1]);  /* parent doesn't write to child's stdout */

    /* Set the read pipe to non-blocking so lsp_read_messages doesn't block */
    int flags = fcntl(from_child[0], F_GETFL, 0);
    fcntl(from_child[0], F_SETFL, flags | O_NONBLOCK);

    LspServer *srv = calloc(1, sizeof(LspServer));
    if (!srv) {
        close(to_child[1]);
        close(from_child[0]);
        kill(pid, SIGTERM);
        return NULL;
    }

    srv->pid         = pid;
    srv->to_server   = to_child[1];
    srv->from_server = from_child[0];
    srv->next_id     = 1;

    lsp_framer_init(&srv->framer);
    lsp_path_to_uri(root_path, srv->root_uri, sizeof(srv->root_uri));

    return srv;
}

void lsp_server_stop(LspServer *srv)
{
    if (!srv) return;

    /* Close pipes */
    if (srv->to_server >= 0) close(srv->to_server);
    if (srv->from_server >= 0) close(srv->from_server);

    /* Try to reap the child process (don't block) */
    if (srv->pid > 0) {
        int status;
        pid_t result = waitpid(srv->pid, &status, WNOHANG);
        if (result == 0) {
            /* Child still running — kill it */
            kill(srv->pid, SIGTERM);
            /* Wait briefly for it to exit */
            usleep(100000);  /* 100ms */
            waitpid(srv->pid, &status, WNOHANG);
        }
    }

    lsp_framer_free(&srv->framer);
    free(srv);
}

/* ============================================================================
 * Message sending
 * ============================================================================ */

/*
 * send_raw — write a framed message to the server's stdin pipe.
 */
static int send_raw(LspServer *srv, const char *json_body)
{
    char *msg = lsp_frame_message(json_body);
    if (!msg) return -1;

    int msg_len = (int)strlen(msg);
    int written = 0;
    while (written < msg_len) {
        int n = (int)write(srv->to_server, msg + written, msg_len - written);
        if (n <= 0) { free(msg); return -1; }
        written += n;
    }

    free(msg);
    return 0;
}

int lsp_send_request(LspServer *srv, const char *method,
                     const char *params_json)
{
    if (!srv || !method) return -1;

    int id = srv->next_id++;

    /*
     * Build the JSON-RPC request:
     * {"jsonrpc":"2.0","id":N,"method":"...","params":{...}}
     */
    char body[65536];
    if (params_json && params_json[0] != '\0')
        snprintf(body, sizeof(body),
                 "{\"jsonrpc\":\"2.0\",\"id\":%d,\"method\":\"%s\",\"params\":%s}",
                 id, method, params_json);
    else
        snprintf(body, sizeof(body),
                 "{\"jsonrpc\":\"2.0\",\"id\":%d,\"method\":\"%s\"}",
                 id, method);

    if (send_raw(srv, body) != 0) return -1;
    return id;
}

int lsp_send_notification(LspServer *srv, const char *method,
                          const char *params_json)
{
    if (!srv || !method) return -1;

    char body[65536];
    if (params_json && params_json[0] != '\0')
        snprintf(body, sizeof(body),
                 "{\"jsonrpc\":\"2.0\",\"method\":\"%s\",\"params\":%s}",
                 method, params_json);
    else
        snprintf(body, sizeof(body),
                 "{\"jsonrpc\":\"2.0\",\"method\":\"%s\"}",
                 method);

    return send_raw(srv, body);
}

/* ============================================================================
 * Message receiving (non-blocking)
 * ============================================================================ */

int lsp_read_messages(LspServer *srv, char **messages, int max_messages)
{
    if (!srv || !messages || max_messages <= 0) return 0;

    /*
     * Read available data from the server's stdout pipe.
     * The pipe is non-blocking (O_NONBLOCK), so read() returns -1 with
     * errno == EAGAIN if no data is available.
     */
    char readbuf[8192];
    for (;;) {
        int n = (int)read(srv->from_server, readbuf, sizeof(readbuf));
        if (n <= 0) break;  /* no more data available */
        lsp_framer_feed(&srv->framer, readbuf, n);
    }

    /* Extract complete messages from the framer */
    int count = 0;
    while (count < max_messages) {
        char *msg = lsp_framer_next(&srv->framer);
        if (!msg) break;
        messages[count++] = msg;
    }

    return count;
}
