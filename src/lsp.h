/*
 * lsp.h — Language Server Protocol Client
 * =============================================================================
 * This module manages communication with a language server process.
 *
 * LSP uses JSON-RPC 2.0 over stdio.  The client (texty) spawns a language
 * server as a child process, communicates via pipes to its stdin/stdout,
 * and exchanges messages with a simple framing protocol:
 *
 *   Content-Length: <N>\r\n
 *   \r\n
 *   <N bytes of JSON>
 *
 * This module handles:
 *   - Message framing (Content-Length headers)
 *   - Process management (fork/exec, pipes, stop/kill)
 *   - Request/response tracking
 *   - LSP protocol messages (initialize, didOpen, completion, etc.)
 *
 * The JSON parsing/building uses json.h.
 * =============================================================================
 */

#ifndef LSP_H
#define LSP_H

#include "json.h"
#include <sys/types.h>   /* pid_t */

/* ---- Constants ------------------------------------------------------------ */

/** Maximum size of an LSP message (16 MB — generous for large completion lists). */
#define LSP_MAX_MSG_SIZE  (16 * 1024 * 1024)

/** Maximum number of pending requests we track for response matching. */
#define LSP_MAX_PENDING   64

/* ---- Message framing ------------------------------------------------------ */

/*
 * LspFramer — accumulates raw bytes and extracts complete LSP messages.
 *
 * LSP messages are framed with "Content-Length: N\r\n\r\n" headers.
 * Data arrives in arbitrary chunks from the pipe, so the framer buffers
 * partial data until a complete message is available.
 *
 * Usage:
 *   1. Feed raw bytes with lsp_framer_feed()
 *   2. Extract complete messages with lsp_framer_next()
 *   3. Repeat until lsp_framer_next() returns NULL
 */
typedef struct {
    char *buf;      /* growable buffer for incoming data   */
    int   len;      /* bytes currently in buffer           */
    int   cap;      /* allocated capacity                  */
} LspFramer;

/**
 * lsp_framer_init — initialize a framer with an empty buffer.
 */
void lsp_framer_init(LspFramer *f);

/**
 * lsp_framer_free — free the framer's buffer.
 */
void lsp_framer_free(LspFramer *f);

/**
 * lsp_framer_feed — append raw bytes to the framer's buffer.
 *
 * Call this when data is read from the language server's stdout pipe.
 * Returns 0 on success, -1 on allocation failure.
 */
int lsp_framer_feed(LspFramer *f, const char *data, int data_len);

/**
 * lsp_framer_next — extract the next complete message from the buffer.
 *
 * If a complete message (Content-Length header + body) is available,
 * returns a heap-allocated string containing the JSON body (caller must
 * free).  Removes the consumed bytes from the framer's buffer.
 *
 * Returns NULL if no complete message is available yet.
 */
char *lsp_framer_next(LspFramer *f);

/* ---- Message building ----------------------------------------------------- */

/**
 * lsp_frame_message — wrap a JSON body with Content-Length headers.
 *
 * Returns a heap-allocated string "Content-Length: N\r\n\r\n<body>"
 * that can be written to the language server's stdin pipe.
 * Caller must free the result.
 */
char *lsp_frame_message(const char *json_body);

/* ---- Diagnostics ---------------------------------------------------------- */

/** Maximum diagnostics per file. */
#define LSP_MAX_DIAGNOSTICS 256

/** LSP diagnostic severity values. */
#define LSP_SEV_ERROR   1
#define LSP_SEV_WARNING 2
#define LSP_SEV_INFO    3
#define LSP_SEV_HINT    4

/*
 * LspDiagnostic — one error/warning reported by the language server.
 */
typedef struct {
    int  line;          /* 0-based line number     */
    int  col;           /* 0-based column          */
    int  end_line;      /* 0-based end line        */
    int  end_col;       /* 0-based end column      */
    int  severity;      /* LSP_SEV_ERROR/WARNING/INFO/HINT */
    char message[256];  /* diagnostic message text  */
} LspDiagnostic;

/*
 * LspDiagnosticList — all diagnostics for one file.
 *
 * Stored per-buffer.  Replaced wholesale when a new publishDiagnostics
 * notification arrives for that file's URI.
 */
typedef struct {
    LspDiagnostic items[LSP_MAX_DIAGNOSTICS];
    int           count;
} LspDiagnosticList;

/**
 * lsp_parse_diagnostics — parse a publishDiagnostics params JSON into a list.
 *
 * `params` is the parsed "params" object from the notification.
 * Populates `out` with the diagnostics array.
 * Returns the number of diagnostics parsed.
 *
 * Pure logic — exposed for testing.
 */
int lsp_parse_diagnostics(LspDiagnosticList *out, const JsonValue *params);

/* ---- LSP server state ----------------------------------------------------- */

/*
 * LspServer — state for one running language server process.
 *
 * Created by lsp_server_start(), freed by lsp_server_stop().
 */
typedef struct LspServer {
    pid_t pid;              /* child process ID                         */
    int   to_server;        /* fd: write to server's stdin              */
    int   from_server;      /* fd: read from server's stdout            */
    LspFramer framer;       /* message framing buffer                   */
    int   next_id;          /* monotonically increasing request ID      */
    int   initialized;      /* 1 after initialize handshake completes   */
    char  root_uri[2048];   /* workspace root as file:// URI            */

    /* Server capabilities (from initialize response) */
    int   has_completion;
    int   has_definition;
    int   has_hover;
    int   has_references;
    int   has_formatting;
    int   has_rename;
    int   has_diagnostics;  /* most servers have this */
} LspServer;

/* ---- Server lifecycle ----------------------------------------------------- */

/**
 * lsp_server_start — spawn a language server and establish pipes.
 *
 * `command` is the shell command to run (e.g. "clangd", "pylsp").
 * `root_path` is the absolute path to the workspace root.
 *
 * Returns a heap-allocated LspServer, or NULL on failure.
 */
LspServer *lsp_server_start(const char *command, const char *root_path);

/**
 * lsp_server_stop — shut down the language server gracefully.
 *
 * Sends the LSP shutdown request, waits briefly, then sends exit.
 * If the server doesn't exit, kills it with SIGTERM.
 * Frees the LspServer struct.
 */
void lsp_server_stop(LspServer *srv);

/* ---- Message sending ------------------------------------------------------ */

/**
 * lsp_send_request — send a JSON-RPC request and return the request ID.
 *
 * `method` is the LSP method name (e.g. "textDocument/completion").
 * `params_json` is the params object as a JSON string (without outer braces
 * of the full message — just the params value).
 *
 * Returns the request ID (>= 0), or -1 on error.
 */
int lsp_send_request(LspServer *srv, const char *method,
                     const char *params_json);

/**
 * lsp_send_notification — send a JSON-RPC notification (no response expected).
 *
 * `method` is the LSP method name (e.g. "textDocument/didOpen").
 * `params_json` is the params value as a JSON string.
 */
int lsp_send_notification(LspServer *srv, const char *method,
                          const char *params_json);

/* ---- Message receiving ---------------------------------------------------- */

/**
 * lsp_read_messages — non-blocking read from the server pipe.
 *
 * Reads available data from the server's stdout, feeds it to the framer,
 * and extracts any complete messages.
 *
 * `messages` is an output array of heap-allocated JSON strings.
 * `max_messages` is the capacity of the array.
 *
 * Returns the number of complete messages read (0 if none available).
 * The caller must free each returned string.
 */
int lsp_read_messages(LspServer *srv, char **messages, int max_messages);

/* ---- Completion ----------------------------------------------------------- */

/** Maximum completion items to display. */
#define LSP_MAX_COMPLETIONS 128

/*
 * LspCompletionItem — one entry from a completion response.
 */
typedef struct {
    char label[256];        /* display text (e.g. "cursor_row")      */
    char insert_text[256];  /* text to insert (may differ from label) */
    char detail[128];       /* type info (e.g. "int")                */
    int  kind;              /* CompletionItemKind (2=method, 3=func..) */
} LspCompletionItem;

/**
 * lsp_parse_completions — parse a completion response into items.
 *
 * `result` is the "result" field from the JSON-RPC response.
 * It can be an array of CompletionItem or a CompletionList object
 * with an "items" array.
 *
 * Returns the number of items parsed (up to LSP_MAX_COMPLETIONS).
 * Pure logic — exposed for testing.
 */
int lsp_parse_completions(LspCompletionItem *out, int max_items,
                          const JsonValue *result);

/* ---- URI helpers ---------------------------------------------------------- */

/**
 * lsp_path_to_uri — convert an absolute file path to a file:// URI.
 *
 * Writes the result into `out`.  Returns 0 on success, -1 on error.
 */
int lsp_path_to_uri(const char *path, char *out, int out_size);

/**
 * lsp_uri_to_path — convert a file:// URI to an absolute file path.
 *
 * Writes the result into `out`.  Returns 0 on success, -1 on error.
 */
int lsp_uri_to_path(const char *uri, char *out, int out_size);

#endif /* LSP_H */
