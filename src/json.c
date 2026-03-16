/*
 * json.c — Minimal JSON Parser Implementation
 * =============================================================================
 * A recursive-descent parser that produces a tree of JsonValue nodes.
 *
 * The parser walks through the input character by character, recognizing
 * JSON values based on the first character:
 *   '"'  → string
 *   '0'-'9' or '-' → number
 *   't' or 'f' → boolean
 *   'n' → null
 *   '{' → object
 *   '[' → array
 *
 * Error handling: on any parse error, the parser returns NULL and frees
 * any partially-built tree.  There is no error message — just NULL.
 *
 * Memory: all nodes, strings, and arrays are heap-allocated.  json_free()
 * recursively frees everything.  The caller only needs to free the root.
 * =============================================================================
 */

#include "json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* ============================================================================
 * Internal: parser state
 * ============================================================================ */

/*
 * Parser state — tracks the current position in the input text.
 * Passed by pointer to all parse functions.
 */
typedef struct {
    const char *text;   /* input string */
    int         pos;    /* current position */
    int         len;    /* total length */
} Parser;

/*
 * skip_ws — advance past whitespace characters.
 */
static void skip_ws(Parser *p)
{
    while (p->pos < p->len) {
        char c = p->text[p->pos];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
            p->pos++;
        else
            break;
    }
}

/*
 * peek — look at the current character without consuming it.
 * Returns 0 if at end of input.
 */
static char peek(Parser *p)
{
    if (p->pos >= p->len) return 0;
    return p->text[p->pos];
}

/*
 * advance — consume and return the current character.
 */
static char advance(Parser *p)
{
    if (p->pos >= p->len) return 0;
    return p->text[p->pos++];
}

/* Forward declaration */
static JsonValue *parse_value(Parser *p);

/* ============================================================================
 * Internal: allocate JsonValue nodes
 * ============================================================================ */

static JsonValue *make_null(void)
{
    JsonValue *v = calloc(1, sizeof(JsonValue));
    if (v) v->type = JSON_NULL;
    return v;
}

static JsonValue *make_bool(int val)
{
    JsonValue *v = calloc(1, sizeof(JsonValue));
    if (v) { v->type = JSON_BOOL; v->bool_val = val; }
    return v;
}

static JsonValue *make_number(double val)
{
    JsonValue *v = calloc(1, sizeof(JsonValue));
    if (v) { v->type = JSON_NUMBER; v->num_val = val; }
    return v;
}

static JsonValue *make_string(const char *s, int len)
{
    JsonValue *v = calloc(1, sizeof(JsonValue));
    if (!v) return NULL;
    v->type = JSON_STRING;
    v->str_val = malloc(len + 1);
    if (!v->str_val) { free(v); return NULL; }
    memcpy(v->str_val, s, len);
    v->str_val[len] = '\0';
    return v;
}

/* ============================================================================
 * Internal: parse individual JSON types
 * ============================================================================ */

/*
 * parse_string — parse a JSON string (the opening '"' has not been consumed).
 *
 * Handles escape sequences: \\ \" \/ \b \f \n \r \t \uXXXX.
 * Returns a JsonValue of type JSON_STRING, or NULL on error.
 */
static JsonValue *parse_string(Parser *p)
{
    if (advance(p) != '"') return NULL;  /* consume opening quote */

    /*
     * Build the unescaped string in a growable buffer.
     * Most strings are short (< 256 bytes), so we start small and grow.
     */
    int cap = 256;
    int len = 0;
    char *buf = malloc(cap);
    if (!buf) return NULL;

    while (p->pos < p->len) {
        char c = advance(p);

        if (c == '"') {
            /* End of string */
            JsonValue *v = make_string(buf, len);
            free(buf);
            return v;
        }

        if (c == '\\') {
            /* Escape sequence */
            char esc = advance(p);
            switch (esc) {
                case '"':  c = '"';  break;
                case '\\': c = '\\'; break;
                case '/':  c = '/';  break;
                case 'b':  c = '\b'; break;
                case 'f':  c = '\f'; break;
                case 'n':  c = '\n'; break;
                case 'r':  c = '\r'; break;
                case 't':  c = '\t'; break;
                case 'u':
                    /*
                     * Unicode escape: \uXXXX.  For ASCII compatibility,
                     * we just store the raw \uXXXX as-is for non-ASCII
                     * codepoints, and decode codepoints < 128 to the
                     * actual character.
                     */
                    if (p->pos + 4 <= p->len) {
                        char hex[5];
                        memcpy(hex, p->text + p->pos, 4);
                        hex[4] = '\0';
                        unsigned int cp = 0;
                        sscanf(hex, "%x", &cp);
                        p->pos += 4;
                        if (cp < 128) {
                            c = (char)cp;
                        } else {
                            /* Non-ASCII: store as ? for simplicity */
                            c = '?';
                        }
                    }
                    break;
                default:
                    /* Unknown escape — keep the character as-is */
                    c = esc;
                    break;
            }
        }

        /* Append character to buffer, growing if needed */
        if (len + 1 >= cap) {
            cap *= 2;
            char *tmp = realloc(buf, cap);
            if (!tmp) { free(buf); return NULL; }
            buf = tmp;
        }
        buf[len++] = c;
    }

    /* Reached end of input without closing quote */
    free(buf);
    return NULL;
}

/*
 * parse_number — parse a JSON number.
 *
 * Handles integers, decimals, and exponential notation.
 * Uses strtod() for the actual conversion.
 */
static JsonValue *parse_number(Parser *p)
{
    int start = p->pos;

    /* Optional leading minus */
    if (peek(p) == '-') p->pos++;

    /* Integer part */
    if (!isdigit((unsigned char)peek(p))) return NULL;
    while (isdigit((unsigned char)peek(p))) p->pos++;

    /* Optional decimal part */
    if (peek(p) == '.') {
        p->pos++;
        while (isdigit((unsigned char)peek(p))) p->pos++;
    }

    /* Optional exponent */
    if (peek(p) == 'e' || peek(p) == 'E') {
        p->pos++;
        if (peek(p) == '+' || peek(p) == '-') p->pos++;
        while (isdigit((unsigned char)peek(p))) p->pos++;
    }

    /* Convert the substring to a double */
    char num_buf[64];
    int num_len = p->pos - start;
    if (num_len >= (int)sizeof(num_buf)) num_len = (int)sizeof(num_buf) - 1;
    memcpy(num_buf, p->text + start, num_len);
    num_buf[num_len] = '\0';

    double val = strtod(num_buf, NULL);
    return make_number(val);
}

/*
 * parse_array — parse a JSON array.
 * The opening '[' has not been consumed.
 */
static JsonValue *parse_array(Parser *p)
{
    if (advance(p) != '[') return NULL;

    JsonValue *v = calloc(1, sizeof(JsonValue));
    if (!v) return NULL;
    v->type = JSON_ARRAY;

    int cap = 8;
    v->array.items = malloc(cap * sizeof(JsonValue *));
    if (!v->array.items) { free(v); return NULL; }
    v->array.count = 0;

    skip_ws(p);
    if (peek(p) == ']') {
        p->pos++;  /* empty array */
        return v;
    }

    for (;;) {
        skip_ws(p);
        JsonValue *item = parse_value(p);
        if (!item) { json_free(v); return NULL; }

        /* Grow the array if needed */
        if (v->array.count >= cap) {
            cap *= 2;
            JsonValue **tmp = realloc(v->array.items,
                                       cap * sizeof(JsonValue *));
            if (!tmp) { json_free(item); json_free(v); return NULL; }
            v->array.items = tmp;
        }
        v->array.items[v->array.count++] = item;

        skip_ws(p);
        if (peek(p) == ',') {
            p->pos++;
            continue;
        }
        if (peek(p) == ']') {
            p->pos++;
            return v;
        }

        /* Unexpected character */
        json_free(v);
        return NULL;
    }
}

/*
 * parse_object — parse a JSON object.
 * The opening '{' has not been consumed.
 */
static JsonValue *parse_object(Parser *p)
{
    if (advance(p) != '{') return NULL;

    JsonValue *v = calloc(1, sizeof(JsonValue));
    if (!v) return NULL;
    v->type = JSON_OBJECT;

    int cap = 8;
    v->object.keys = malloc(cap * sizeof(char *));
    v->object.vals = malloc(cap * sizeof(JsonValue *));
    if (!v->object.keys || !v->object.vals) { json_free(v); return NULL; }
    v->object.count = 0;

    skip_ws(p);
    if (peek(p) == '}') {
        p->pos++;  /* empty object */
        return v;
    }

    for (;;) {
        skip_ws(p);

        /* Parse the key (must be a string) */
        if (peek(p) != '"') { json_free(v); return NULL; }
        JsonValue *key_val = parse_string(p);
        if (!key_val || key_val->type != JSON_STRING) {
            json_free(key_val);
            json_free(v);
            return NULL;
        }

        /* Expect ':' between key and value */
        skip_ws(p);
        if (advance(p) != ':') {
            json_free(key_val);
            json_free(v);
            return NULL;
        }

        /* Parse the value */
        skip_ws(p);
        JsonValue *val = parse_value(p);
        if (!val) {
            json_free(key_val);
            json_free(v);
            return NULL;
        }

        /* Grow arrays if needed */
        if (v->object.count >= cap) {
            cap *= 2;
            char **tk = realloc(v->object.keys, cap * sizeof(char *));
            JsonValue **tv = realloc(v->object.vals,
                                      cap * sizeof(JsonValue *));
            if (!tk || !tv) {
                json_free(key_val);
                json_free(val);
                json_free(v);
                return NULL;
            }
            v->object.keys = tk;
            v->object.vals = tv;
        }

        /* Transfer key ownership: steal the string from key_val */
        v->object.keys[v->object.count] = key_val->str_val;
        key_val->str_val = NULL;  /* prevent double free */
        json_free(key_val);

        v->object.vals[v->object.count] = val;
        v->object.count++;

        skip_ws(p);
        if (peek(p) == ',') {
            p->pos++;
            continue;
        }
        if (peek(p) == '}') {
            p->pos++;
            return v;
        }

        /* Unexpected character */
        json_free(v);
        return NULL;
    }
}

/*
 * parse_value — parse any JSON value based on the first character.
 */
static JsonValue *parse_value(Parser *p)
{
    skip_ws(p);
    char c = peek(p);

    if (c == '"') return parse_string(p);
    if (c == '{') return parse_object(p);
    if (c == '[') return parse_array(p);

    if (c == '-' || isdigit((unsigned char)c))
        return parse_number(p);

    /* true / false / null */
    if (p->pos + 4 <= p->len && strncmp(p->text + p->pos, "true", 4) == 0) {
        p->pos += 4;
        return make_bool(1);
    }
    if (p->pos + 5 <= p->len && strncmp(p->text + p->pos, "false", 5) == 0) {
        p->pos += 5;
        return make_bool(0);
    }
    if (p->pos + 4 <= p->len && strncmp(p->text + p->pos, "null", 4) == 0) {
        p->pos += 4;
        return make_null();
    }

    return NULL;  /* unrecognized */
}

/* ============================================================================
 * Public API: parsing
 * ============================================================================ */

JsonValue *json_parse(const char *text, int len)
{
    if (!text) return NULL;
    if (len < 0) len = (int)strlen(text);
    if (len == 0) return NULL;

    Parser p = { text, 0, len };
    JsonValue *v = parse_value(&p);

    /* Verify no trailing garbage (except whitespace) */
    if (v) {
        skip_ws(&p);
        /* Trailing content is OK — LSP messages are self-contained */
    }

    return v;
}

void json_free(JsonValue *v)
{
    if (!v) return;

    switch (v->type) {
    case JSON_STRING:
        free(v->str_val);
        break;
    case JSON_ARRAY:
        for (int i = 0; i < v->array.count; i++)
            json_free(v->array.items[i]);
        free(v->array.items);
        break;
    case JSON_OBJECT:
        for (int i = 0; i < v->object.count; i++) {
            free(v->object.keys[i]);
            json_free(v->object.vals[i]);
        }
        free(v->object.keys);
        free(v->object.vals);
        break;
    default:
        break;
    }

    free(v);
}

/* ============================================================================
 * Public API: navigation
 * ============================================================================ */

JsonValue *json_get(const JsonValue *v, const char *key)
{
    if (!v || !key || v->type != JSON_OBJECT) return NULL;

    for (int i = 0; i < v->object.count; i++) {
        if (strcmp(v->object.keys[i], key) == 0)
            return v->object.vals[i];
    }
    return NULL;
}

const char *json_get_string(const JsonValue *v)
{
    if (!v || v->type != JSON_STRING) return NULL;
    return v->str_val;
}

int json_get_int(const JsonValue *v, int fallback)
{
    if (!v || v->type != JSON_NUMBER) return fallback;
    return (int)v->num_val;
}

int json_get_bool(const JsonValue *v, int fallback)
{
    if (!v || v->type != JSON_BOOL) return fallback;
    return v->bool_val;
}

JsonValue *json_array_get(const JsonValue *v, int index)
{
    if (!v || v->type != JSON_ARRAY) return NULL;
    if (index < 0 || index >= v->array.count) return NULL;
    return v->array.items[index];
}

int json_array_len(const JsonValue *v)
{
    if (!v || v->type != JSON_ARRAY) return 0;
    return v->array.count;
}

/* ============================================================================
 * Public API: string escaping (for building outbound JSON)
 * ============================================================================ */

int json_escape_string(const char *src, int src_len,
                       char *out, int out_size)
{
    if (!src || !out || out_size <= 0) return -1;
    if (src_len < 0) src_len = (int)strlen(src);

    int j = 0;  /* output position */

    for (int i = 0; i < src_len; i++) {
        unsigned char c = (unsigned char)src[i];
        const char *esc = NULL;

        switch (c) {
            case '"':  esc = "\\\""; break;
            case '\\': esc = "\\\\"; break;
            case '\n': esc = "\\n";  break;
            case '\r': esc = "\\r";  break;
            case '\t': esc = "\\t";  break;
            case '\b': esc = "\\b";  break;
            case '\f': esc = "\\f";  break;
            default:
                if (c < 0x20) {
                    /* Control character → \uXXXX */
                    if (j + 6 >= out_size) return -1;
                    j += snprintf(out + j, out_size - j, "\\u%04x", c);
                    continue;
                }
                /* Regular character — copy as-is */
                if (j + 1 >= out_size) return -1;
                out[j++] = (char)c;
                continue;
        }

        /* Write the 2-character escape sequence */
        int elen = (int)strlen(esc);
        if (j + elen >= out_size) return -1;
        memcpy(out + j, esc, elen);
        j += elen;
    }

    out[j] = '\0';
    return j;
}
