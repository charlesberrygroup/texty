/*
 * test_json.c — Unit tests for json.c
 * =============================================================================
 * Tests the JSON parser, navigation functions, and string escaping.
 * All pure logic — no ncurses dependency.
 * =============================================================================
 */

#include "test_runner.h"
#include "json.h"

#include <stdlib.h>
#include <string.h>

/* ============================================================================
 * Parsing basics
 * ============================================================================ */

TEST(test_parse_null_input)
{
    ASSERT(json_parse(NULL, -1) == NULL, "NULL input");
    ASSERT(json_parse("", -1) == NULL, "empty input");
}

TEST(test_parse_null_value)
{
    JsonValue *v = json_parse("null", -1);
    ASSERT(v != NULL, "parsed");
    ASSERT(v->type == JSON_NULL, "type is null");
    json_free(v);
}

TEST(test_parse_true)
{
    JsonValue *v = json_parse("true", -1);
    ASSERT(v != NULL, "parsed");
    ASSERT(v->type == JSON_BOOL, "type is bool");
    ASSERT(v->bool_val == 1, "value is true");
    json_free(v);
}

TEST(test_parse_false)
{
    JsonValue *v = json_parse("false", -1);
    ASSERT(v != NULL, "parsed");
    ASSERT(v->type == JSON_BOOL, "type is bool");
    ASSERT(v->bool_val == 0, "value is false");
    json_free(v);
}

TEST(test_parse_integer)
{
    JsonValue *v = json_parse("42", -1);
    ASSERT(v != NULL, "parsed");
    ASSERT(v->type == JSON_NUMBER, "type is number");
    ASSERT((int)v->num_val == 42, "value is 42");
    json_free(v);
}

TEST(test_parse_negative)
{
    JsonValue *v = json_parse("-7", -1);
    ASSERT(v != NULL, "parsed");
    ASSERT((int)v->num_val == -7, "value is -7");
    json_free(v);
}

TEST(test_parse_decimal)
{
    JsonValue *v = json_parse("3.14", -1);
    ASSERT(v != NULL, "parsed");
    ASSERT(v->num_val > 3.13 && v->num_val < 3.15, "value is ~3.14");
    json_free(v);
}

TEST(test_parse_string)
{
    JsonValue *v = json_parse("\"hello\"", -1);
    ASSERT(v != NULL, "parsed");
    ASSERT(v->type == JSON_STRING, "type is string");
    ASSERT(strcmp(v->str_val, "hello") == 0, "value is 'hello'");
    json_free(v);
}

TEST(test_parse_string_escapes)
{
    JsonValue *v = json_parse("\"line1\\nline2\\ttab\\\\back\"", -1);
    ASSERT(v != NULL, "parsed");
    ASSERT(strstr(v->str_val, "line1\nline2\ttab\\back") != NULL,
           "escape sequences decoded");
    json_free(v);
}

TEST(test_parse_string_unicode_escape)
{
    JsonValue *v = json_parse("\"\\u0041\"", -1);  /* \u0041 = 'A' */
    ASSERT(v != NULL, "parsed");
    ASSERT(v->str_val[0] == 'A', "\\u0041 decoded to 'A'");
    json_free(v);
}

TEST(test_parse_empty_string)
{
    JsonValue *v = json_parse("\"\"", -1);
    ASSERT(v != NULL, "parsed");
    ASSERT(v->type == JSON_STRING, "type is string");
    ASSERT(v->str_val[0] == '\0', "empty string");
    json_free(v);
}

/* ============================================================================
 * Arrays
 * ============================================================================ */

TEST(test_parse_empty_array)
{
    JsonValue *v = json_parse("[]", -1);
    ASSERT(v != NULL, "parsed");
    ASSERT(v->type == JSON_ARRAY, "type is array");
    ASSERT(v->array.count == 0, "empty");
    json_free(v);
}

TEST(test_parse_array_of_numbers)
{
    JsonValue *v = json_parse("[1, 2, 3]", -1);
    ASSERT(v != NULL, "parsed");
    ASSERT(v->type == JSON_ARRAY, "type is array");
    ASSERT(v->array.count == 3, "three elements");
    ASSERT((int)v->array.items[0]->num_val == 1, "first = 1");
    ASSERT((int)v->array.items[2]->num_val == 3, "third = 3");
    json_free(v);
}

TEST(test_parse_nested_array)
{
    JsonValue *v = json_parse("[[1, 2], [3]]", -1);
    ASSERT(v != NULL, "parsed");
    ASSERT(v->array.count == 2, "two sub-arrays");
    ASSERT(v->array.items[0]->array.count == 2, "first has 2");
    ASSERT(v->array.items[1]->array.count == 1, "second has 1");
    json_free(v);
}

TEST(test_parse_mixed_array)
{
    JsonValue *v = json_parse("[1, \"two\", true, null]", -1);
    ASSERT(v != NULL, "parsed");
    ASSERT(v->array.count == 4, "four elements");
    ASSERT(v->array.items[0]->type == JSON_NUMBER, "first is number");
    ASSERT(v->array.items[1]->type == JSON_STRING, "second is string");
    ASSERT(v->array.items[2]->type == JSON_BOOL, "third is bool");
    ASSERT(v->array.items[3]->type == JSON_NULL, "fourth is null");
    json_free(v);
}

/* ============================================================================
 * Objects
 * ============================================================================ */

TEST(test_parse_empty_object)
{
    JsonValue *v = json_parse("{}", -1);
    ASSERT(v != NULL, "parsed");
    ASSERT(v->type == JSON_OBJECT, "type is object");
    ASSERT(v->object.count == 0, "empty");
    json_free(v);
}

TEST(test_parse_simple_object)
{
    JsonValue *v = json_parse("{\"name\": \"texty\", \"version\": 1}", -1);
    ASSERT(v != NULL, "parsed");
    ASSERT(v->type == JSON_OBJECT, "type is object");
    ASSERT(v->object.count == 2, "two keys");
    json_free(v);
}

TEST(test_parse_nested_object)
{
    const char *json =
        "{\"result\": {\"items\": [1, 2]}, \"id\": 5}";
    JsonValue *v = json_parse(json, -1);
    ASSERT(v != NULL, "parsed");

    JsonValue *result = json_get(v, "result");
    ASSERT(result != NULL, "has result");
    ASSERT(result->type == JSON_OBJECT, "result is object");

    JsonValue *items = json_get(result, "items");
    ASSERT(items != NULL, "has items");
    ASSERT(items->type == JSON_ARRAY, "items is array");
    ASSERT(items->array.count == 2, "items has 2 elements");

    json_free(v);
}

/* ============================================================================
 * Navigation helpers
 * ============================================================================ */

TEST(test_json_get)
{
    JsonValue *v = json_parse("{\"a\": 1, \"b\": \"two\"}", -1);
    ASSERT(v != NULL, "parsed");

    JsonValue *a = json_get(v, "a");
    ASSERT(a != NULL, "found 'a'");
    ASSERT(json_get_int(a, 0) == 1, "a = 1");

    JsonValue *b = json_get(v, "b");
    ASSERT(b != NULL, "found 'b'");
    ASSERT(strcmp(json_get_string(b), "two") == 0, "b = 'two'");

    ASSERT(json_get(v, "c") == NULL, "missing key returns NULL");

    json_free(v);
}

TEST(test_json_get_string_null)
{
    ASSERT(json_get_string(NULL) == NULL, "NULL value");
    JsonValue *v = json_parse("42", -1);
    ASSERT(json_get_string(v) == NULL, "number is not string");
    json_free(v);
}

TEST(test_json_get_int_fallback)
{
    ASSERT(json_get_int(NULL, 99) == 99, "NULL returns fallback");
    JsonValue *v = json_parse("\"text\"", -1);
    ASSERT(json_get_int(v, 99) == 99, "string returns fallback");
    json_free(v);
}

TEST(test_json_get_bool_fallback)
{
    ASSERT(json_get_bool(NULL, 0) == 0, "NULL returns fallback");
}

TEST(test_json_array_get)
{
    JsonValue *v = json_parse("[10, 20, 30]", -1);
    ASSERT(json_get_int(json_array_get(v, 0), 0) == 10, "index 0");
    ASSERT(json_get_int(json_array_get(v, 2), 0) == 30, "index 2");
    ASSERT(json_array_get(v, 3) == NULL, "out of bounds");
    ASSERT(json_array_get(v, -1) == NULL, "negative index");
    json_free(v);
}

TEST(test_json_array_len)
{
    JsonValue *v = json_parse("[1, 2]", -1);
    ASSERT(json_array_len(v) == 2, "length 2");
    json_free(v);

    ASSERT(json_array_len(NULL) == 0, "NULL returns 0");
}

/* ============================================================================
 * LSP-like JSON
 * ============================================================================ */

TEST(test_parse_lsp_response)
{
    /*
     * A simplified LSP initialize response.
     */
    const char *json =
        "{"
        "  \"jsonrpc\": \"2.0\","
        "  \"id\": 1,"
        "  \"result\": {"
        "    \"capabilities\": {"
        "      \"completionProvider\": {"
        "        \"triggerCharacters\": [\".\", \">\"]"
        "      },"
        "      \"textDocumentSync\": 1,"
        "      \"hoverProvider\": true"
        "    }"
        "  }"
        "}";

    JsonValue *v = json_parse(json, -1);
    ASSERT(v != NULL, "parsed LSP response");

    ASSERT(json_get_int(json_get(v, "id"), 0) == 1, "id = 1");

    JsonValue *result = json_get(v, "result");
    ASSERT(result != NULL, "has result");

    JsonValue *caps = json_get(result, "capabilities");
    ASSERT(caps != NULL, "has capabilities");

    JsonValue *hover = json_get(caps, "hoverProvider");
    ASSERT(hover != NULL, "has hoverProvider");
    ASSERT(json_get_bool(hover, 0) == 1, "hoverProvider = true");

    JsonValue *sync = json_get(caps, "textDocumentSync");
    ASSERT(json_get_int(sync, 0) == 1, "textDocumentSync = 1");

    JsonValue *comp = json_get(caps, "completionProvider");
    ASSERT(comp != NULL, "has completionProvider");
    JsonValue *triggers = json_get(comp, "triggerCharacters");
    ASSERT(json_array_len(triggers) == 2, "2 trigger chars");

    json_free(v);
}

TEST(test_parse_diagnostics_notification)
{
    const char *json =
        "{"
        "  \"jsonrpc\": \"2.0\","
        "  \"method\": \"textDocument/publishDiagnostics\","
        "  \"params\": {"
        "    \"uri\": \"file:///src/main.c\","
        "    \"diagnostics\": ["
        "      {"
        "        \"range\": {"
        "          \"start\": {\"line\": 5, \"character\": 3},"
        "          \"end\": {\"line\": 5, \"character\": 10}"
        "        },"
        "        \"severity\": 1,"
        "        \"message\": \"undeclared identifier\""
        "      }"
        "    ]"
        "  }"
        "}";

    JsonValue *v = json_parse(json, -1);
    ASSERT(v != NULL, "parsed diagnostics");

    const char *method = json_get_string(json_get(v, "method"));
    ASSERT(method != NULL, "has method");
    ASSERT(strcmp(method, "textDocument/publishDiagnostics") == 0, "method name");

    JsonValue *params = json_get(v, "params");
    JsonValue *diags = json_get(params, "diagnostics");
    ASSERT(json_array_len(diags) == 1, "one diagnostic");

    JsonValue *d = json_array_get(diags, 0);
    ASSERT(json_get_int(json_get(d, "severity"), 0) == 1, "severity 1");

    const char *msg = json_get_string(json_get(d, "message"));
    ASSERT(msg != NULL && strcmp(msg, "undeclared identifier") == 0, "message");

    JsonValue *range = json_get(d, "range");
    JsonValue *start = json_get(range, "start");
    ASSERT(json_get_int(json_get(start, "line"), -1) == 5, "line 5");
    ASSERT(json_get_int(json_get(start, "character"), -1) == 3, "char 3");

    json_free(v);
}

/* ============================================================================
 * String escaping
 * ============================================================================ */

TEST(test_escape_simple)
{
    char out[256];
    int n = json_escape_string("hello", -1, out, sizeof(out));
    ASSERT(n > 0, "escape succeeded");
    ASSERT(strcmp(out, "hello") == 0, "no escaping needed");
}

TEST(test_escape_special_chars)
{
    char out[256];
    json_escape_string("line1\nline2\ttab", -1, out, sizeof(out));
    ASSERT(strstr(out, "\\n") != NULL, "newline escaped");
    ASSERT(strstr(out, "\\t") != NULL, "tab escaped");
}

TEST(test_escape_quotes)
{
    char out[256];
    json_escape_string("say \"hi\"", -1, out, sizeof(out));
    ASSERT(strstr(out, "\\\"") != NULL, "quotes escaped");
}

TEST(test_escape_backslash)
{
    char out[256];
    json_escape_string("path\\to\\file", -1, out, sizeof(out));
    ASSERT(strstr(out, "\\\\") != NULL, "backslash escaped");
}

TEST(test_escape_control_char)
{
    char src[2] = { 0x01, 0 };
    char out[256];
    json_escape_string(src, 1, out, sizeof(out));
    ASSERT(strstr(out, "\\u0001") != NULL, "control char escaped as \\uXXXX");
}

TEST(test_escape_buffer_too_small)
{
    char out[5];
    int n = json_escape_string("this is too long", -1, out, sizeof(out));
    ASSERT(n == -1, "returns -1 when buffer too small");
}

TEST(test_escape_null_input)
{
    char out[32];
    ASSERT(json_escape_string(NULL, 0, out, sizeof(out)) == -1, "NULL input");
}

/* ============================================================================
 * json_free safety
 * ============================================================================ */

TEST(test_free_null)
{
    json_free(NULL);  /* should not crash */
    ASSERT(1, "free(NULL) is safe");
}

/* ============================================================================
 * main
 * ============================================================================ */

int main(void)
{
    printf("=== test_json ===\n");

    /* Parsing basics */
    RUN(test_parse_null_input);
    RUN(test_parse_null_value);
    RUN(test_parse_true);
    RUN(test_parse_false);
    RUN(test_parse_integer);
    RUN(test_parse_negative);
    RUN(test_parse_decimal);
    RUN(test_parse_string);
    RUN(test_parse_string_escapes);
    RUN(test_parse_string_unicode_escape);
    RUN(test_parse_empty_string);

    /* Arrays */
    RUN(test_parse_empty_array);
    RUN(test_parse_array_of_numbers);
    RUN(test_parse_nested_array);
    RUN(test_parse_mixed_array);

    /* Objects */
    RUN(test_parse_empty_object);
    RUN(test_parse_simple_object);
    RUN(test_parse_nested_object);

    /* Navigation */
    RUN(test_json_get);
    RUN(test_json_get_string_null);
    RUN(test_json_get_int_fallback);
    RUN(test_json_get_bool_fallback);
    RUN(test_json_array_get);
    RUN(test_json_array_len);

    /* LSP-like JSON */
    RUN(test_parse_lsp_response);
    RUN(test_parse_diagnostics_notification);

    /* String escaping */
    RUN(test_escape_simple);
    RUN(test_escape_special_chars);
    RUN(test_escape_quotes);
    RUN(test_escape_backslash);
    RUN(test_escape_control_char);
    RUN(test_escape_buffer_too_small);
    RUN(test_escape_null_input);

    /* Safety */
    RUN(test_free_null);

    TEST_SUMMARY();
}
