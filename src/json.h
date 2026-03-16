/*
 * json.h — Minimal JSON Parser
 * =============================================================================
 * A simple recursive-descent JSON parser for the LSP protocol.
 *
 * This is NOT a full JSON library — it's just enough to parse LSP responses
 * and build LSP requests.  It handles:
 *   - Strings (with escape sequences: \n, \t, \\, \", \uXXXX)
 *   - Numbers (integers and decimals)
 *   - Booleans (true/false)
 *   - null
 *   - Objects (key-value pairs)
 *   - Arrays
 *
 * The parser produces a tree of JsonValue nodes.  Navigation functions
 * (json_get, json_array_get) let you drill into the tree without manual
 * pointer chasing.
 *
 * For BUILDING JSON (outbound LSP messages), use snprintf() with templates
 * and json_escape_string() for safe string escaping.  This is simpler than
 * building a tree and serializing it.
 *
 * This module has NO ncurses dependency — fully testable.
 * =============================================================================
 */

#ifndef JSON_H
#define JSON_H

/* ---- Data Types ----------------------------------------------------------- */

/*
 * JsonType — what kind of value this node holds.
 */
typedef enum {
    JSON_NULL,
    JSON_BOOL,
    JSON_NUMBER,
    JSON_STRING,
    JSON_ARRAY,
    JSON_OBJECT
} JsonType;

/*
 * JsonValue — one node in the parsed JSON tree.
 *
 * Heap-allocated.  Freed recursively with json_free().
 * Access the value through the union, guided by the `type` field.
 *
 * For objects, keys[i] pairs with vals[i].
 * For arrays, items[i] is the i-th element.
 */
typedef struct JsonValue {
    JsonType type;
    union {
        int     bool_val;                   /* JSON_BOOL   */
        double  num_val;                    /* JSON_NUMBER  */
        char   *str_val;                    /* JSON_STRING (heap-allocated) */
        struct {
            struct JsonValue **items;       /* JSON_ARRAY   */
            int count;
        } array;
        struct {
            char **keys;                    /* JSON_OBJECT (heap-allocated keys) */
            struct JsonValue **vals;        /* JSON_OBJECT (heap-allocated vals) */
            int count;
        } object;
    };
} JsonValue;

/* ---- Parsing -------------------------------------------------------------- */

/**
 * json_parse — parse a JSON string into a tree of JsonValue nodes.
 *
 * Returns the root node (heap-allocated), or NULL on parse error.
 * The caller must free the result with json_free().
 *
 * `text` does not need to be null-terminated if `len` is provided.
 * Pass len=-1 to use strlen(text).
 */
JsonValue *json_parse(const char *text, int len);

/**
 * json_free — recursively free a JsonValue tree.
 *
 * Safe to call on NULL.
 */
void json_free(JsonValue *v);

/* ---- Navigation ----------------------------------------------------------- */

/**
 * json_get — look up a key in a JSON object.
 *
 * Returns the value for `key`, or NULL if not found or `v` is not an object.
 * The returned pointer is owned by the parent — do NOT free it separately.
 */
JsonValue *json_get(const JsonValue *v, const char *key);

/**
 * json_get_string — extract a string value, or NULL.
 *
 * Returns the string pointer (owned by the JsonValue) if v is a JSON_STRING,
 * or NULL otherwise.
 */
const char *json_get_string(const JsonValue *v);

/**
 * json_get_int — extract an integer value, or `fallback`.
 *
 * Returns the number cast to int if v is JSON_NUMBER, or `fallback` otherwise.
 */
int json_get_int(const JsonValue *v, int fallback);

/**
 * json_get_bool — extract a boolean value, or `fallback`.
 */
int json_get_bool(const JsonValue *v, int fallback);

/**
 * json_array_get — get the i-th element of a JSON array.
 *
 * Returns NULL if v is not an array or i is out of bounds.
 */
JsonValue *json_array_get(const JsonValue *v, int index);

/**
 * json_array_len — get the length of a JSON array.
 *
 * Returns 0 if v is not an array.
 */
int json_array_len(const JsonValue *v);

/* ---- String escaping (for building outbound JSON) ------------------------- */

/**
 * json_escape_string — escape a string for safe embedding in JSON.
 *
 * Writes the escaped string (WITHOUT surrounding quotes) into `out`.
 * Returns the number of bytes written, or -1 if the buffer is too small.
 *
 * Escapes: \\ \" \n \r \t \b \f and control characters as \uXXXX.
 */
int json_escape_string(const char *src, int src_len,
                       char *out, int out_size);

#endif /* JSON_H */
