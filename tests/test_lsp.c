/*
 * test_lsp.c — Unit tests for lsp.c
 * =============================================================================
 * Tests the LSP message framing, URI conversion, and message building.
 * Does NOT spawn actual language servers — only pure-logic functions.
 * =============================================================================
 */

#include "test_runner.h"
#include "lsp.h"
#include "json.h"

#include <stdlib.h>
#include <string.h>

/* ============================================================================
 * Message framing
 * ============================================================================ */

TEST(test_framer_init_free)
{
    LspFramer f;
    lsp_framer_init(&f);
    ASSERT(f.buf == NULL, "empty after init");
    ASSERT(f.len == 0, "len 0");
    lsp_framer_free(&f);
    ASSERT(1, "init/free cycle OK");
}

TEST(test_framer_complete_message)
{
    LspFramer f;
    lsp_framer_init(&f);

    const char *json = "{\"id\":1,\"r\":2}";
    int json_len = (int)strlen(json);  /* 14 */
    char msg[128];
    snprintf(msg, sizeof(msg), "Content-Length: %d\r\n\r\n%s", json_len, json);
    lsp_framer_feed(&f, msg, (int)strlen(msg));

    char *body = lsp_framer_next(&f);
    ASSERT(body != NULL, "complete message extracted");
    ASSERT(strcmp(body, json) == 0, "body correct");
    free(body);

    /* No more messages */
    ASSERT(lsp_framer_next(&f) == NULL, "no second message");

    lsp_framer_free(&f);
}

TEST(test_framer_partial_header)
{
    LspFramer f;
    lsp_framer_init(&f);

    /* Feed only the header, no body yet */
    const char *part1 = "Content-Length: 5\r\n\r\n";
    lsp_framer_feed(&f, part1, (int)strlen(part1));

    ASSERT(lsp_framer_next(&f) == NULL, "no message yet (body missing)");

    /* Now feed the body */
    const char *part2 = "hello";
    lsp_framer_feed(&f, part2, (int)strlen(part2));

    char *body = lsp_framer_next(&f);
    ASSERT(body != NULL, "message after body arrives");
    ASSERT(strcmp(body, "hello") == 0, "body is 'hello'");
    free(body);

    lsp_framer_free(&f);
}

TEST(test_framer_split_in_header)
{
    LspFramer f;
    lsp_framer_init(&f);

    /* Feed the header in two chunks */
    lsp_framer_feed(&f, "Content-Le", 10);
    ASSERT(lsp_framer_next(&f) == NULL, "not yet");

    lsp_framer_feed(&f, "ngth: 3\r\n\r\nabc", 14);
    char *body = lsp_framer_next(&f);
    ASSERT(body != NULL, "message extracted after second chunk");
    ASSERT(strcmp(body, "abc") == 0, "body is 'abc'");
    free(body);

    lsp_framer_free(&f);
}

TEST(test_framer_two_messages)
{
    LspFramer f;
    lsp_framer_init(&f);

    const char *data =
        "Content-Length: 3\r\n\r\nabc"
        "Content-Length: 5\r\n\r\nhello";
    lsp_framer_feed(&f, data, (int)strlen(data));

    char *m1 = lsp_framer_next(&f);
    ASSERT(m1 != NULL, "first message");
    ASSERT(strcmp(m1, "abc") == 0, "first body");
    free(m1);

    char *m2 = lsp_framer_next(&f);
    ASSERT(m2 != NULL, "second message");
    ASSERT(strcmp(m2, "hello") == 0, "second body");
    free(m2);

    ASSERT(lsp_framer_next(&f) == NULL, "no third message");

    lsp_framer_free(&f);
}

TEST(test_framer_byte_by_byte)
{
    LspFramer f;
    lsp_framer_init(&f);

    const char *msg = "Content-Length: 2\r\n\r\nOK";
    int msg_len = (int)strlen(msg);

    /* Feed one byte at a time */
    for (int i = 0; i < msg_len - 1; i++) {
        lsp_framer_feed(&f, msg + i, 1);
        ASSERT(lsp_framer_next(&f) == NULL, "not complete yet");
    }

    /* Feed last byte */
    lsp_framer_feed(&f, msg + msg_len - 1, 1);
    char *body = lsp_framer_next(&f);
    ASSERT(body != NULL, "complete after last byte");
    ASSERT(strcmp(body, "OK") == 0, "body is 'OK'");
    free(body);

    lsp_framer_free(&f);
}

TEST(test_framer_empty_feed)
{
    LspFramer f;
    lsp_framer_init(&f);

    ASSERT(lsp_framer_feed(&f, NULL, 0) == 0, "NULL feed is OK");
    ASSERT(lsp_framer_feed(&f, "x", 0) == 0, "zero-length feed is OK");
    ASSERT(lsp_framer_next(&f) == NULL, "no message");

    lsp_framer_free(&f);
}

TEST(test_framer_json_body)
{
    LspFramer f;
    lsp_framer_init(&f);

    /* A realistic JSON-RPC response */
    const char *json = "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":null}";
    int json_len = (int)strlen(json);

    char header[64];
    int hdr_len = snprintf(header, sizeof(header),
                           "Content-Length: %d\r\n\r\n", json_len);

    lsp_framer_feed(&f, header, hdr_len);
    lsp_framer_feed(&f, json, json_len);

    char *body = lsp_framer_next(&f);
    ASSERT(body != NULL, "message extracted");
    ASSERT(strcmp(body, json) == 0, "JSON body preserved exactly");
    free(body);

    lsp_framer_free(&f);
}

/* ============================================================================
 * Message building
 * ============================================================================ */

TEST(test_frame_message)
{
    char *framed = lsp_frame_message("{\"test\":1}");
    ASSERT(framed != NULL, "framed message built");
    ASSERT(strstr(framed, "Content-Length: 10") != NULL, "has header");
    ASSERT(strstr(framed, "\r\n\r\n") != NULL, "has separator");
    ASSERT(strstr(framed, "{\"test\":1}") != NULL, "has body");
    free(framed);
}

TEST(test_frame_message_null)
{
    ASSERT(lsp_frame_message(NULL) == NULL, "NULL body");
}

/* ============================================================================
 * URI conversion
 * ============================================================================ */

TEST(test_path_to_uri)
{
    char uri[256];
    lsp_path_to_uri("/home/user/file.c", uri, sizeof(uri));
    ASSERT(strcmp(uri, "file:///home/user/file.c") == 0, "path → URI");
}

TEST(test_uri_to_path)
{
    char path[256];
    lsp_uri_to_path("file:///home/user/file.c", path, sizeof(path));
    ASSERT(strcmp(path, "/home/user/file.c") == 0, "URI → path");
}

TEST(test_uri_roundtrip)
{
    char uri[256], path[256];
    lsp_path_to_uri("/src/main.c", uri, sizeof(uri));
    lsp_uri_to_path(uri, path, sizeof(path));
    ASSERT(strcmp(path, "/src/main.c") == 0, "roundtrip preserves path");
}

TEST(test_uri_not_file)
{
    char path[256];
    lsp_uri_to_path("https://example.com", path, sizeof(path));
    ASSERT(strcmp(path, "https://example.com") == 0,
           "non-file URI copied as-is");
}

TEST(test_uri_null_input)
{
    char out[256];
    ASSERT(lsp_path_to_uri(NULL, out, sizeof(out)) == -1, "NULL path");
    ASSERT(lsp_uri_to_path(NULL, out, sizeof(out)) == -1, "NULL uri");
}

/* ============================================================================
 * Diagnostic parsing
 * ============================================================================ */

TEST(test_parse_diagnostics_basic)
{
    /* Build a publishDiagnostics params as a JSON string, then parse */
    const char *json =
        "{"
        "  \"uri\": \"file:///src/main.c\","
        "  \"diagnostics\": ["
        "    {"
        "      \"range\": {"
        "        \"start\": {\"line\": 5, \"character\": 3},"
        "        \"end\": {\"line\": 5, \"character\": 10}"
        "      },"
        "      \"severity\": 1,"
        "      \"message\": \"undeclared identifier\""
        "    },"
        "    {"
        "      \"range\": {"
        "        \"start\": {\"line\": 10, \"character\": 0},"
        "        \"end\": {\"line\": 10, \"character\": 5}"
        "      },"
        "      \"severity\": 2,"
        "      \"message\": \"unused variable\""
        "    }"
        "  ]"
        "}";

    JsonValue *params = json_parse(json, -1);
    ASSERT(params != NULL, "parsed params");

    LspDiagnosticList list;
    memset(&list, 0, sizeof(list));
    int n = lsp_parse_diagnostics(&list, params);

    ASSERT(n == 2, "two diagnostics");
    ASSERT(list.count == 2, "count is 2");

    ASSERT(list.items[0].line == 5, "first diag line");
    ASSERT(list.items[0].col == 3, "first diag col");
    ASSERT(list.items[0].severity == 1, "first is error");
    ASSERT(strcmp(list.items[0].message, "undeclared identifier") == 0,
           "first message");

    ASSERT(list.items[1].line == 10, "second diag line");
    ASSERT(list.items[1].severity == 2, "second is warning");
    ASSERT(strcmp(list.items[1].message, "unused variable") == 0,
           "second message");

    json_free(params);
}

TEST(test_parse_diagnostics_empty)
{
    const char *json =
        "{\"uri\": \"file:///f.c\", \"diagnostics\": []}";

    JsonValue *params = json_parse(json, -1);
    LspDiagnosticList list;
    memset(&list, 0, sizeof(list));
    int n = lsp_parse_diagnostics(&list, params);

    ASSERT(n == 0, "zero diagnostics");
    ASSERT(list.count == 0, "count is 0");

    json_free(params);
}

TEST(test_parse_diagnostics_null)
{
    LspDiagnosticList list;
    memset(&list, 0, sizeof(list));
    ASSERT(lsp_parse_diagnostics(&list, NULL) == 0, "NULL params → 0");
    ASSERT(lsp_parse_diagnostics(NULL, NULL) == 0, "both NULL → 0");
}

/* ============================================================================
 * Completion parsing
 * ============================================================================ */

TEST(test_parse_completions_array)
{
    const char *json =
        "["
        "  {\"label\": \"cursor_row\", \"kind\": 5, \"detail\": \"int\"},"
        "  {\"label\": \"cursor_col\", \"kind\": 5, \"detail\": \"int\"},"
        "  {\"label\": \"clipboard\", \"insertText\": \"clipboard\", \"kind\": 5}"
        "]";

    JsonValue *result = json_parse(json, -1);
    ASSERT(result != NULL, "parsed");

    LspCompletionItem items[16];
    int n = lsp_parse_completions(items, 16, result);
    ASSERT(n == 3, "three items");

    ASSERT(strcmp(items[0].label, "cursor_row") == 0, "first label");
    ASSERT(strcmp(items[0].detail, "int") == 0, "first detail");
    ASSERT(items[0].kind == 5, "first kind");

    ASSERT(strcmp(items[2].label, "clipboard") == 0, "third label");
    ASSERT(strcmp(items[2].insert_text, "clipboard") == 0,
           "insert_text falls back to label");

    json_free(result);
}

TEST(test_parse_completions_list_object)
{
    /* CompletionList format (with isIncomplete flag) */
    const char *json =
        "{\"isIncomplete\": false, \"items\": ["
        "  {\"label\": \"printf\", \"kind\": 3, \"detail\": \"int printf(...)\"}"
        "]}";

    JsonValue *result = json_parse(json, -1);
    LspCompletionItem items[16];
    int n = lsp_parse_completions(items, 16, result);
    ASSERT(n == 1, "one item from CompletionList");
    ASSERT(strcmp(items[0].label, "printf") == 0, "label");
    json_free(result);
}

TEST(test_parse_completions_empty)
{
    JsonValue *result = json_parse("[]", -1);
    LspCompletionItem items[16];
    int n = lsp_parse_completions(items, 16, result);
    ASSERT(n == 0, "empty array → 0 items");
    json_free(result);
}

TEST(test_parse_completions_null)
{
    LspCompletionItem items[16];
    ASSERT(lsp_parse_completions(items, 16, NULL) == 0, "NULL → 0");
    ASSERT(lsp_parse_completions(NULL, 16, NULL) == 0, "both NULL → 0");
}

/* ============================================================================
 * main
 * ============================================================================ */

int main(void)
{
    printf("=== test_lsp ===\n");

    /* Message framing */
    RUN(test_framer_init_free);
    RUN(test_framer_complete_message);
    RUN(test_framer_partial_header);
    RUN(test_framer_split_in_header);
    RUN(test_framer_two_messages);
    RUN(test_framer_byte_by_byte);
    RUN(test_framer_empty_feed);
    RUN(test_framer_json_body);

    /* Message building */
    RUN(test_frame_message);
    RUN(test_frame_message_null);

    /* URI conversion */
    RUN(test_path_to_uri);
    RUN(test_uri_to_path);
    RUN(test_uri_roundtrip);
    RUN(test_uri_not_file);
    RUN(test_uri_null_input);

    /* Diagnostic parsing */
    RUN(test_parse_diagnostics_basic);
    RUN(test_parse_diagnostics_empty);
    RUN(test_parse_diagnostics_null);

    /* Completion parsing */
    RUN(test_parse_completions_array);
    RUN(test_parse_completions_list_object);
    RUN(test_parse_completions_empty);
    RUN(test_parse_completions_null);

    TEST_SUMMARY();
}
