/*
 * syntax.c — Syntax Highlighting Implementation
 * =============================================================================
 * See syntax.h for the public API.
 *
 * Architecture
 * ------------
 * The core of this file is highlight_generic(), a single tokenizer that
 * handles C, C++, JavaScript, TypeScript, Rust, Go, and Shell — languages
 * that share the same basic token types but differ in their keyword lists
 * and comment styles.  A LangConfig struct captures those differences.
 *
 * Python and JSON have unique enough rules to warrant their own tokenizers.
 * Markdown and Makefile also have their own simpler tokenizers.
 *
 * All tokenizers follow the same contract: they fill tokens[0..len-1] and
 * return the multi-line state to carry into the next line.
 *
 * There are no dependencies on ncurses or any terminal library — this file
 * can be compiled and tested without a display.
 * =============================================================================
 */

#include "syntax.h"

#include <string.h>   /* strlen, strrchr, strcmp, strncmp, memset */
#include <ctype.h>    /* isalpha, isalnum, isdigit, isspace       */

/* =============================================================================
 * Helper macros
 * ============================================================================= */

/*
 * IS_WORD_START — true for characters that can begin an identifier.
 * In C and most languages: letters and underscore.
 */
#define IS_WORD_START(c)  (isalpha((unsigned char)(c)) || (c) == '_')

/*
 * IS_WORD_CONT — true for characters that can continue an identifier.
 * In C and most languages: letters, digits, and underscore.
 */
#define IS_WORD_CONT(c)   (isalnum((unsigned char)(c)) || (c) == '_')

/*
 * IS_DIGIT — true for ASCII decimal digits (0-9).
 */
#define IS_DIGIT(c)       isdigit((unsigned char)(c))

/* =============================================================================
 * Keyword / type lists
 *
 * Each list is a NULL-terminated array of C strings.  The tokenizer does a
 * linear search through the list for each identifier it encounters.  The lists
 * are short enough (< 100 entries) that a linear scan is faster than a hash
 * table due to cache locality.
 * ============================================================================= */

/* ---- C / C++ -------------------------------------------------------------- */

static const char *C_KEYWORDS[] = {
    /* C control flow */
    "break", "case", "continue", "default", "do", "else", "for",
    "goto", "if", "return", "switch", "while",
    /* C storage / linkage / qualifiers */
    "auto", "const", "extern", "inline", "register", "restrict",
    "static", "volatile",
    /* C types and type-related */
    "enum", "sizeof", "struct", "typedef", "union",
    /* C++ specific */
    "catch", "class", "delete", "explicit", "friend", "mutable",
    "namespace", "new", "operator", "override", "private", "protected",
    "public", "template", "this", "throw", "try", "using", "virtual",
    NULL
};

static const char *C_TYPES[] = {
    /* Primitive types */
    "char", "double", "float", "int", "long", "short",
    "signed", "unsigned", "void",
    /* C++ Boolean */
    "bool", "true", "false", "nullptr",
    /* C99 / C11 */
    "NULL", "_Bool", "_Complex",
    /* Fixed-width integers from stdint.h */
    "int8_t",   "int16_t",  "int32_t",  "int64_t",
    "uint8_t",  "uint16_t", "uint32_t", "uint64_t",
    "intptr_t", "uintptr_t",
    /* Common typedef names */
    "size_t", "ssize_t", "ptrdiff_t", "off_t",
    NULL
};

/* ---- Python --------------------------------------------------------------- */

static const char *PY_KEYWORDS[] = {
    "and", "as", "assert", "async", "await",
    "break", "class", "continue", "def", "del",
    "elif", "else", "except", "finally", "for",
    "from", "global", "if", "import", "in",
    "is", "lambda", "nonlocal", "not", "or",
    "pass", "raise", "return", "try", "while",
    "with", "yield",
    /* Boolean / None literals */
    "True", "False", "None",
    NULL
};

static const char *PY_TYPES[] = {
    /* Built-in types used as names */
    "int", "float", "str", "bool", "bytes",
    "list", "dict", "set", "tuple", "type",
    "object", "Exception",
    NULL
};

/* ---- JavaScript / TypeScript ---------------------------------------------- */

static const char *JS_KEYWORDS[] = {
    /* ES6+ control flow */
    "break", "case", "catch", "continue", "debugger",
    "default", "delete", "do", "else", "finally",
    "for", "function", "if", "in", "instanceof",
    "new", "return", "switch", "this", "throw",
    "try", "typeof", "var", "void", "while",
    "with", "yield",
    /* ES6 */
    "class", "const", "export", "extends", "import",
    "let", "static", "super",
    /* Async */
    "async", "await",
    /* Literals */
    "true", "false", "null", "undefined",
    /* TypeScript-specific */
    "abstract", "as", "declare", "enum", "from",
    "implements", "interface", "module", "namespace",
    "of", "private", "protected", "public", "readonly",
    "type",
    NULL
};

static const char *JS_TYPES[] = {
    /* TypeScript primitive types */
    "string", "number", "boolean", "object", "symbol",
    "bigint", "any", "unknown", "never", "void",
    /* Common built-ins */
    "Array", "Object", "Promise", "Error",
    NULL
};

/* ---- Rust ----------------------------------------------------------------- */

static const char *RUST_KEYWORDS[] = {
    "as", "async", "await", "break", "const", "continue",
    "crate", "dyn", "else", "enum", "extern", "false",
    "fn", "for", "if", "impl", "in", "let", "loop",
    "match", "mod", "move", "mut", "pub", "ref",
    "return", "self", "Self", "static", "struct", "super",
    "trait", "true", "type", "unsafe", "use", "where",
    "while",
    NULL
};

static const char *RUST_TYPES[] = {
    /* Primitive types */
    "bool", "char", "str",
    "i8",  "i16",  "i32",  "i64",  "i128",  "isize",
    "u8",  "u16",  "u32",  "u64",  "u128",  "usize",
    "f32", "f64",
    /* Common standard types */
    "String", "Vec", "Option", "Result", "Box",
    NULL
};

/* ---- Go ------------------------------------------------------------------- */

static const char *GO_KEYWORDS[] = {
    "break", "case", "chan", "const", "continue",
    "default", "defer", "else", "fallthrough", "for",
    "func", "go", "goto", "if", "import",
    "interface", "map", "package", "range", "return",
    "select", "struct", "switch", "type", "var",
    NULL
};

static const char *GO_TYPES[] = {
    /* Predeclared types */
    "bool", "byte", "complex64", "complex128", "error",
    "float32", "float64",
    "int", "int8", "int16", "int32", "int64",
    "rune", "string",
    "uint", "uint8", "uint16", "uint32", "uint64", "uintptr",
    /* Predeclared values */
    "true", "false", "nil", "iota",
    NULL
};

/* ---- Shell script --------------------------------------------------------- */

static const char *SH_KEYWORDS[] = {
    "if", "then", "else", "elif", "fi",
    "for", "do", "done", "while", "until",
    "case", "esac", "in", "select",
    "function", "return", "exit",
    "export", "local", "readonly", "unset",
    "echo", "printf", "read", "shift", "source",
    NULL
};

/* =============================================================================
 * LangConfig — parameters for the generic C-like tokenizer.
 *
 * Most popular languages share the same basic token structure (identifiers,
 * strings, numbers, comments) but differ in details.  This struct captures
 * those differences so one tokenizer can serve them all.
 * ============================================================================= */
typedef struct {
    /*
     * Keyword and type lists.  Both are NULL-terminated arrays of C strings,
     * or NULL if the language doesn't distinguish keywords from identifiers.
     */
    const char **keywords;   /* colored SYN_KEYWORD (if, for, return...)    */
    const char **types;      /* colored SYN_TYPE    (int, void, bool...)    */

    /*
     * Which comment styles does this language support?
     *
     * has_line_comment   — 1 if "//" starts a line comment  (C, JS, Rust, Go)
     * has_block_comment  — 1 if slash-star...star-slash is a block comment
     * has_hash_comment   — 1 if "#" starts a line comment   (Python, Shell)
     *
     * has_preproc is intentionally separate from has_hash_comment:
     *   - In C/C++, "#" introduces a preprocessor directive (SYN_PREPROC).
     *     The rest of the line is colored PREPROC, not COMMENT.
     *   - In Python/Shell, "#" starts a line comment (SYN_COMMENT).
     */
    int has_line_comment;    /* 1 → // is a line comment                    */
    int has_block_comment;   /* 1 → slash-star...star-slash block comment    */
    int has_hash_comment;    /* 1 → # starts a line comment                 */
    int has_preproc;         /* 1 → lines starting with # are SYN_PREPROC   */

    /*
     * has_backtick_string — 1 if backtick `...` is a string literal.
     * True for JavaScript template literals; false for all other languages.
     */
    int has_backtick_string;
} LangConfig;

/* Per-language config instances */
static const LangConfig CFG_C = {
    C_KEYWORDS, C_TYPES,
    /*has_line=*/1, /*has_block=*/1, /*has_hash_cmt=*/0,
    /*has_preproc=*/1, /*has_backtick=*/0
};

static const LangConfig CFG_JS = {
    JS_KEYWORDS, JS_TYPES,
    /*has_line=*/1, /*has_block=*/1, /*has_hash_cmt=*/0,
    /*has_preproc=*/0, /*has_backtick=*/1
};

static const LangConfig CFG_RUST = {
    RUST_KEYWORDS, RUST_TYPES,
    /*has_line=*/1, /*has_block=*/1, /*has_hash_cmt=*/0,
    /*has_preproc=*/0, /*has_backtick=*/0
};

static const LangConfig CFG_GO = {
    GO_KEYWORDS, GO_TYPES,
    /*has_line=*/1, /*has_block=*/1, /*has_hash_cmt=*/0,
    /*has_preproc=*/0, /*has_backtick=*/0
};

static const LangConfig CFG_SHELL = {
    SH_KEYWORDS, NULL,
    /*has_line=*/0, /*has_block=*/0, /*has_hash_cmt=*/1,
    /*has_preproc=*/0, /*has_backtick=*/0
};

/* =============================================================================
 * Helper: match_word
 *
 * Returns 1 if the word (text[0..len-1]) exactly matches any entry in the
 * NULL-terminated array `words`; returns 0 otherwise.
 *
 * We use memcmp instead of strcmp because `text` may not be NUL-terminated
 * (it could be a slice of a longer line).
 * ============================================================================= */
static int match_word(const char **words, const char *text, int len)
{
    if (words == NULL || len <= 0) return 0;
    for (int i = 0; words[i] != NULL; i++) {
        /*
         * Check length first: strlen is fast, and an early-out length mismatch
         * avoids calling memcmp for most entries.
         */
        if ((int)strlen(words[i]) == len &&
            memcmp(words[i], text, (size_t)len) == 0) {
            return 1;
        }
    }
    return 0;
}

/* =============================================================================
 * Helper: scan_string
 *
 * Scans a quoted string literal starting at text[i], where text[i] is the
 * opening quote character `quote_char` (either '"' or '\'').  Fills
 * tokens[i..end] with SYN_STRING, handling backslash escapes correctly.
 *
 * Returns the index of the character AFTER the closing quote (or `len` if
 * the string runs to end of line without closing).
 *
 * C example:  "hello\nworld"
 *             ^             ^ returns index after this
 * ============================================================================= */
static int scan_string(const char *text, int len, SyntaxToken *tokens,
                       int i, char quote_char)
{
    /*
     * Mark the opening quote, then advance past it.
     */
    tokens[i++] = SYN_STRING;

    while (i < len && text[i] != quote_char) {
        tokens[i] = SYN_STRING;

        if (text[i] == '\\' && i + 1 < len) {
            /*
             * Escape sequence: the backslash AND the next character both
             * belong to the string (e.g. \" does not end the string).
             */
            tokens[i + 1] = SYN_STRING;
            i += 2;
        } else {
            i++;
        }
    }

    /* Mark the closing quote if present */
    if (i < len) tokens[i++] = SYN_STRING;

    return i;
}

/* =============================================================================
 * highlight_generic — tokenizer for C-like languages.
 *
 * Handles all languages described by a LangConfig.  Returns the multi-line
 * state for the next line (0 = normal, 1 = inside block comment).
 * ============================================================================= */
static int highlight_generic(const LangConfig *cfg,
                              const char *text, int len,
                              SyntaxToken *tokens, int ml_state)
{
    int i = 0;

    /* Initialize all characters to SYN_NORMAL.
     * memset is efficient here: SYN_NORMAL == 0, so we can zero the array. */
    memset(tokens, SYN_NORMAL, (size_t)len);

    /* -----------------------------------------------------------------------
     * Handle lines that begin INSIDE a block comment.
     *
     * If the previous line ended with an unclosed block comment, we enter this line
     * in ml_state == 1.  We mark characters as SYN_COMMENT until we find
     * the closing "*\/", then resume normal scanning.
     * ----------------------------------------------------------------------- */
    if (ml_state == 1 && cfg->has_block_comment) {
        while (i < len) {
            tokens[i] = SYN_COMMENT;
            if (i + 1 < len && text[i] == '*' && text[i + 1] == '/') {
                tokens[i + 1] = SYN_COMMENT;
                i += 2;
                ml_state = 0;
                break;  /* comment closed; fall through to normal scanning */
            }
            i++;
        }
        if (ml_state == 1) {
            return 1;  /* still inside block comment at end of line */
        }
    }

    /* -----------------------------------------------------------------------
     * Preprocessor detection (C/C++ only).
     *
     * A line is a preprocessor directive if "#" appears as the first
     * non-whitespace character.  When true, all "ordinary" characters on the
     * line are colored SYN_PREPROC instead of SYN_NORMAL.  String literals
     * within the directive (#include "foo.h") are still colored SYN_STRING.
     * ----------------------------------------------------------------------- */
    int is_preproc = 0;
    if (cfg->has_preproc) {
        int j = 0;
        while (j < len && (text[j] == ' ' || text[j] == '\t')) j++;
        is_preproc = (j < len && text[j] == '#');
    }

    /*
     * base_tok — the color to use for unclassified characters on this line.
     * On a preprocessor line everything defaults to PREPROC; elsewhere NORMAL.
     */
    SyntaxToken base_tok = is_preproc ? SYN_PREPROC : SYN_NORMAL;

    /* -----------------------------------------------------------------------
     * Main scan loop — process characters left to right.
     * ----------------------------------------------------------------------- */
    while (i < len) {
        char c = text[i];

        /* ---- Line comment: "//" ----------------------------------------- */
        if (cfg->has_line_comment &&
            i + 1 < len && c == '/' && text[i + 1] == '/') {
            /*
             * Everything from here to end of line is a comment.
             * We use a simple loop instead of memset because SYN_COMMENT != 0.
             */
            for (int k = i; k < len; k++) tokens[k] = SYN_COMMENT;
            return 0;   /* no multi-line state from a line comment */
        }

        /* ---- Hash comment: "#" (Python, Shell) --------------------------- */
        if (cfg->has_hash_comment && c == '#') {
            for (int k = i; k < len; k++) tokens[k] = SYN_COMMENT;
            return 0;
        }

        /* ---- Block comment: slash-star ... star-slash -------------------- */
        if (cfg->has_block_comment &&
            i + 1 < len && c == '/' && text[i + 1] == '*') {
            int closed = 0;
            tokens[i++] = SYN_COMMENT;
            tokens[i++] = SYN_COMMENT;

            while (i < len) {
                tokens[i] = SYN_COMMENT;
                if (i + 1 < len && text[i] == '*' && text[i + 1] == '/') {
                    tokens[i + 1] = SYN_COMMENT;
                    i += 2;
                    closed = 1;
                    break;
                }
                i++;
            }

            if (!closed) {
                return 1;   /* block comment continues on the next line */
            }
            continue;   /* comment closed; keep scanning rest of line */
        }

        /* ---- String literal: "..." --------------------------------------- */
        if (c == '"') {
            i = scan_string(text, len, tokens, i, '"');
            continue;
        }

        /* ---- Character / string literal: '...' -------------------------- */
        if (c == '\'') {
            i = scan_string(text, len, tokens, i, '\'');
            continue;
        }

        /* ---- Template literal: `...` (JavaScript) ----------------------- */
        if (cfg->has_backtick_string && c == '`') {
            i = scan_string(text, len, tokens, i, '`');
            continue;
        }

        /* ---- Numeric literal: digits, hex (0x...), floats --------------- */
        if (IS_DIGIT(c)) {
            /*
             * Consume digits plus '.', 'x', 'b', 'e', '_' (covers decimal,
             * float, hex 0xFF, binary 0b1010, Rust's 1_000_000, etc.).
             * We don't need to be perfectly precise — coloring "0xGG" as a
             * number is a minor cosmetic issue, not a correctness bug.
             */
            while (i < len &&
                   (IS_DIGIT(text[i]) || isalpha((unsigned char)text[i]) ||
                    text[i] == '.' || text[i] == '_')) {
                tokens[i++] = SYN_NUMBER;
            }
            continue;
        }

        /* ---- Identifier, keyword, or type name -------------------------- */
        if (IS_WORD_START(c)) {
            int start = i;
            while (i < len && IS_WORD_CONT(text[i])) i++;

            int wlen = i - start;
            SyntaxToken tok = base_tok;  /* default: NORMAL or PREPROC */

            /*
             * Only check keywords/types on non-preprocessor lines.
             * On a preproc line like "#ifdef HAVE_FOO", HAVE_FOO is just
             * a macro name — coloring it as a keyword would look wrong.
             */
            if (!is_preproc) {
                if (cfg->keywords && match_word(cfg->keywords, text + start, wlen)) {
                    tok = SYN_KEYWORD;
                } else if (cfg->types && match_word(cfg->types, text + start, wlen)) {
                    tok = SYN_TYPE;
                }
            }

            for (int k = start; k < i; k++) tokens[k] = tok;
            continue;
        }

        /* ---- Everything else (operators, punctuation, whitespace) ------- */
        tokens[i++] = base_tok;
    }

    return 0;   /* no ongoing multi-line state */
}

/* =============================================================================
 * highlight_python — tokenizer for Python.
 *
 * Python differs from C-like languages in three ways:
 *   1. # starts a line comment (not a preprocessor directive).
 *   2. Triple-quoted strings ("""...""" and '''...''') can span multiple lines.
 *   3. No block comments.
 *
 * ml_state values:
 *   0 = normal
 *   2 = inside """...""" triple-double-quoted string
 *   3 = inside '''...''' triple-single-quoted string
 * ============================================================================= */
static int highlight_python(const char *text, int len,
                             SyntaxToken *tokens, int ml_state)
{
    int i = 0;
    memset(tokens, SYN_NORMAL, (size_t)len);

    /* -----------------------------------------------------------------------
     * Handle lines entering inside a triple-quoted string.
     * ----------------------------------------------------------------------- */
    if (ml_state == 2 || ml_state == 3) {
        char q = (ml_state == 2) ? '"' : '\'';

        while (i < len) {
            tokens[i] = SYN_STRING;
            /* Look for three consecutive quote characters to close the string */
            if (i + 2 < len &&
                text[i] == q && text[i + 1] == q && text[i + 2] == q) {
                tokens[i + 1] = SYN_STRING;
                tokens[i + 2] = SYN_STRING;
                i += 3;
                ml_state = 0;
                break;
            }
            i++;
        }
        if (ml_state != 0) {
            return ml_state;  /* still inside triple-quoted string */
        }
    }

    /* -----------------------------------------------------------------------
     * Main scan loop.
     * ----------------------------------------------------------------------- */
    while (i < len) {
        char c = text[i];

        /* ---- Hash comment: # --------------------------------------------- */
        if (c == '#') {
            for (int k = i; k < len; k++) tokens[k] = SYN_COMMENT;
            return 0;
        }

        /* ---- Triple-double-quote: """...""" ------------------------------ */
        if (c == '"' && i + 2 < len &&
            text[i + 1] == '"' && text[i + 2] == '"') {

            tokens[i++] = SYN_STRING;
            tokens[i++] = SYN_STRING;
            tokens[i++] = SYN_STRING;

            int closed = 0;
            while (i < len) {
                tokens[i] = SYN_STRING;
                if (i + 2 < len &&
                    text[i] == '"' && text[i + 1] == '"' && text[i + 2] == '"') {
                    tokens[i + 1] = SYN_STRING;
                    tokens[i + 2] = SYN_STRING;
                    i += 3;
                    closed = 1;
                    break;
                }
                i++;
            }
            if (!closed) return 2;  /* continues on the next line */
            continue;
        }

        /* ---- Triple-single-quote: '''...''' ------------------------------ */
        if (c == '\'' && i + 2 < len &&
            text[i + 1] == '\'' && text[i + 2] == '\'') {

            tokens[i++] = SYN_STRING;
            tokens[i++] = SYN_STRING;
            tokens[i++] = SYN_STRING;

            int closed = 0;
            while (i < len) {
                tokens[i] = SYN_STRING;
                if (i + 2 < len &&
                    text[i] == '\'' && text[i + 1] == '\'' && text[i + 2] == '\'') {
                    tokens[i + 1] = SYN_STRING;
                    tokens[i + 2] = SYN_STRING;
                    i += 3;
                    closed = 1;
                    break;
                }
                i++;
            }
            if (!closed) return 3;  /* continues on the next line */
            continue;
        }

        /* ---- Regular double-quoted string: "..." ------------------------- */
        if (c == '"') {
            i = scan_string(text, len, tokens, i, '"');
            continue;
        }

        /* ---- Regular single-quoted string: '...' ------------------------- */
        if (c == '\'') {
            i = scan_string(text, len, tokens, i, '\'');
            continue;
        }

        /* ---- Numeric literal --------------------------------------------- */
        if (IS_DIGIT(c)) {
            while (i < len &&
                   (IS_DIGIT(text[i]) || isalpha((unsigned char)text[i]) ||
                    text[i] == '.' || text[i] == '_')) {
                tokens[i++] = SYN_NUMBER;
            }
            continue;
        }

        /* ---- Identifier / keyword --------------------------------------- */
        if (IS_WORD_START(c)) {
            int start = i;
            while (i < len && IS_WORD_CONT(text[i])) i++;
            int wlen = i - start;

            SyntaxToken tok = SYN_NORMAL;
            if (match_word(PY_KEYWORDS, text + start, wlen)) {
                tok = SYN_KEYWORD;
            } else if (match_word(PY_TYPES, text + start, wlen)) {
                tok = SYN_TYPE;
            }
            for (int k = start; k < i; k++) tokens[k] = tok;
            continue;
        }

        /* ---- Everything else --------------------------------------------- */
        tokens[i++] = SYN_NORMAL;
    }

    return 0;
}

/* =============================================================================
 * highlight_json — tokenizer for JSON.
 *
 * JSON has:
 *   - String literals: "..."
 *   - Numeric literals: -42, 3.14, 1e10
 *   - Keywords: true, false, null
 *   - No comments (standard JSON doesn't allow comments)
 * ============================================================================= */
static int highlight_json(const char *text, int len,
                           SyntaxToken *tokens, int ml_state)
{
    int i = 0;
    memset(tokens, SYN_NORMAL, (size_t)len);
    (void)ml_state;  /* JSON has no multi-line constructs */

    while (i < len) {
        char c = text[i];

        /* ---- String: "..." ----------------------------------------------- */
        if (c == '"') {
            i = scan_string(text, len, tokens, i, '"');
            continue;
        }

        /* ---- Number: optional '-', then digits --------------------------- */
        if (IS_DIGIT(c) || (c == '-' && i + 1 < len && IS_DIGIT(text[i + 1]))) {
            tokens[i++] = SYN_NUMBER;   /* the '-' or first digit */
            while (i < len &&
                   (IS_DIGIT(text[i]) || text[i] == '.' ||
                    text[i] == 'e' || text[i] == 'E' ||
                    text[i] == '+' || text[i] == '-')) {
                tokens[i++] = SYN_NUMBER;
            }
            continue;
        }

        /* ---- Keywords: true, false, null --------------------------------- */
        if (IS_WORD_START(c)) {
            int start = i;
            while (i < len && IS_WORD_CONT(text[i])) i++;
            int wlen = i - start;

            SyntaxToken tok = SYN_NORMAL;
            if (wlen == 4 && memcmp(text + start, "true",  4) == 0) tok = SYN_KEYWORD;
            if (wlen == 5 && memcmp(text + start, "false", 5) == 0) tok = SYN_KEYWORD;
            if (wlen == 4 && memcmp(text + start, "null",  4) == 0) tok = SYN_KEYWORD;
            for (int k = start; k < i; k++) tokens[k] = tok;
            continue;
        }

        tokens[i++] = SYN_NORMAL;
    }

    return 0;
}

/* =============================================================================
 * highlight_markdown — tokenizer for Markdown.
 *
 * Markdown is rendered, not executed, so the "token types" are a bit
 * different — we color structural elements rather than language semantics:
 *
 *   SYN_KEYWORD   — headings:  lines starting with one or more '#'
 *   SYN_COMMENT   — block code: lines starting with 4 spaces or a tab,
 *                   and fenced code blocks (``` or ~~~)
 *   SYN_STRING    — inline code: `...`
 *   SYN_PREPROC   — horizontal rules, list markers (-  *  +  1.)
 *   SYN_NORMAL    — regular paragraph text
 * ============================================================================= */
static int highlight_markdown(const char *text, int len,
                               SyntaxToken *tokens, int ml_state)
{
    int i = 0;
    memset(tokens, SYN_NORMAL, (size_t)len);

    /*
     * ml_state for Markdown:
     *   0 = normal  1 = inside a fenced code block (``` or ~~~)
     */
    if (ml_state == 1) {
        /* Inside fenced code block — check if this line closes it */
        int j = 0;
        int fence_char = 0;
        while (j < len && (text[j] == ' ' || text[j] == '\t')) j++;
        if (j < len) fence_char = text[j];

        if ((fence_char == '`' || fence_char == '~') &&
            j + 2 < len &&
            text[j + 1] == fence_char && text[j + 2] == fence_char) {
            /* Closing fence — color it and leave fenced mode */
            for (int k = 0; k < len; k++) tokens[k] = SYN_COMMENT;
            return 0;
        }
        /* Still inside fence — color whole line as code */
        for (int k = 0; k < len; k++) tokens[k] = SYN_COMMENT;
        return 1;
    }

    /* Skip leading whitespace to find the first non-blank character */
    int j = 0;
    while (j < len && (text[j] == ' ' || text[j] == '\t')) j++;
    if (j >= len) return 0;   /* blank line */

    char first = text[j];

    /* ---- Fenced code block: ``` or ~~~ ---------------------------------- */
    if ((first == '`' || first == '~') &&
        j + 2 < len && text[j + 1] == first && text[j + 2] == first) {
        for (int k = 0; k < len; k++) tokens[k] = SYN_COMMENT;
        return 1;   /* entering fenced code block */
    }

    /* ---- Indented code block: 4+ spaces or a tab ------------------------ */
    if ((j >= 4 && text[0] == ' ') || text[0] == '\t') {
        for (int k = 0; k < len; k++) tokens[k] = SYN_COMMENT;
        return 0;
    }

    /* ---- Heading: one or more '#' at start ------------------------------- */
    if (first == '#') {
        for (int k = 0; k < len; k++) tokens[k] = SYN_KEYWORD;
        return 0;
    }

    /* ---- Horizontal rule / list marker ---------------------------------- */
    if (first == '-' || first == '*' || first == '+') {
        /* Mark just the marker character; leave the rest as normal */
        tokens[j] = SYN_PREPROC;
        i = j + 1;
    }
    /* Numbered list: digit(s) followed by '.' */
    else if (IS_DIGIT(first)) {
        int k = j;
        while (k < len && IS_DIGIT(text[k])) k++;
        if (k < len && text[k] == '.') {
            for (int m = j; m <= k; m++) tokens[m] = SYN_PREPROC;
            i = k + 1;
        }
    }

    /* ---- Inline code: `...` --------------------------------------------- */
    while (i < len) {
        if (text[i] == '`') {
            tokens[i++] = SYN_STRING;
            while (i < len && text[i] != '`') tokens[i++] = SYN_STRING;
            if (i < len) tokens[i++] = SYN_STRING;
        } else {
            i++;
        }
    }

    return 0;
}

/* =============================================================================
 * highlight_makefile — tokenizer for Makefiles.
 *
 *   SYN_COMMENT  — lines starting with '#'
 *   SYN_KEYWORD  — target names (text before ':' on a non-recipe line)
 *   SYN_PREPROC  — automatic variables and make directives ($@, $<, include...)
 *   SYN_STRING   — variable references: $(VAR) and ${VAR}
 *   SYN_NORMAL   — recipe commands and values
 * ============================================================================= */
static int highlight_makefile(const char *text, int len,
                               SyntaxToken *tokens, int ml_state)
{
    int i = 0;
    memset(tokens, SYN_NORMAL, (size_t)len);
    (void)ml_state;

    if (len == 0) return 0;

    /* ---- Comment: # ------------------------------------------------------ */
    if (text[0] == '#') {
        for (int k = 0; k < len; k++) tokens[k] = SYN_COMMENT;
        return 0;
    }

    /* ---- Recipe line: starts with a TAB ---------------------------------- */
    if (text[0] == '\t') {
        /* Recipe lines are commands; leave them mostly normal but color
         * variable references and automatic variables. */
        i = 1;
        /* Fall through to the variable-reference scanning below */
    } else {
        /* ---- Target: text before the first ':' on a rule line ----------- */
        int colon = -1;
        for (int k = 0; k < len; k++) {
            if (text[k] == ':') { colon = k; break; }
        }
        if (colon > 0) {
            /* Color target name(s) as SYN_KEYWORD */
            for (int k = 0; k < colon; k++) tokens[k] = SYN_KEYWORD;
            tokens[colon] = SYN_PREPROC;  /* the ':' itself */
            i = colon + 1;
        }

        /* ---- Makefile directives at start of line (include, define...) -- */
        static const char *MAKE_DIRECTIVES[] = {
            "include", "define", "endef", "ifdef", "ifndef",
            "ifeq", "ifneq", "else", "endif", "export",
            "unexport", "override", "vpath", NULL
        };
        if (colon < 0) {
            /* Not a rule line — check for directives */
            int j = 0;
            while (j < len && IS_WORD_START(text[j])) j++;
            if (match_word(MAKE_DIRECTIVES, text, j)) {
                for (int k = 0; k < j; k++) tokens[k] = SYN_KEYWORD;
                i = j;
            }
        }
    }

    /* ---- Variable references: $(VAR) ${VAR} $@ $< etc. ------------------ */
    while (i < len) {
        if (text[i] == '$') {
            tokens[i++] = SYN_STRING;
            if (i < len) {
                if (text[i] == '(' || text[i] == '{') {
                    char close = (text[i] == '(') ? ')' : '}';
                    tokens[i++] = SYN_STRING;
                    while (i < len && text[i] != close) tokens[i++] = SYN_STRING;
                    if (i < len) tokens[i++] = SYN_STRING;
                } else {
                    /* Automatic variable: $@, $<, $^, $*, etc. */
                    tokens[i++] = SYN_STRING;
                }
            }
        } else if (text[i] == '#') {
            /* Inline comment */
            for (int k = i; k < len; k++) tokens[k] = SYN_COMMENT;
            break;
        } else {
            i++;
        }
    }

    return 0;
}

/* =============================================================================
 * syntax_detect_language — public API
 *
 * Checks the file extension by finding the last '.' in the basename.
 * Also handles special cases like "Makefile" and "makefile" with no extension.
 * ============================================================================= */
SyntaxLang syntax_detect_language(const char *filename)
{
    if (filename == NULL) return LANG_NONE;

    /*
     * Find the basename: the part of the path after the last '/'.
     * If there is no '/', the whole string is the basename.
     */
    const char *base = strrchr(filename, '/');
    base = base ? base + 1 : filename;

    /* ---- Special filenames without extensions ---------------------------- */
    if (strcmp(base, "Makefile")    == 0 ||
        strcmp(base, "makefile")    == 0 ||
        strcmp(base, "GNUmakefile") == 0) {
        return LANG_MAKEFILE;
    }

    /* ---- Find the extension (last '.' in the basename) ------------------- */
    const char *dot = strrchr(base, '.');
    if (dot == NULL || dot == base) {
        return LANG_NONE;   /* no extension, or filename starts with '.' */
    }
    const char *ext = dot + 1;   /* pointer to the extension string */

    /* ---- Match extensions ------------------------------------------------ */

    /* C / C++ */
    if (strcmp(ext, "c")   == 0 || strcmp(ext, "h")   == 0 ||
        strcmp(ext, "cpp") == 0 || strcmp(ext, "cc")  == 0 ||
        strcmp(ext, "cxx") == 0 || strcmp(ext, "hpp") == 0) {
        return LANG_C;
    }

    /* Python */
    if (strcmp(ext, "py") == 0 || strcmp(ext, "pyw") == 0) {
        return LANG_PYTHON;
    }

    /* JavaScript / TypeScript */
    if (strcmp(ext, "js")  == 0 || strcmp(ext, "ts")  == 0 ||
        strcmp(ext, "jsx") == 0 || strcmp(ext, "tsx") == 0 ||
        strcmp(ext, "mjs") == 0 || strcmp(ext, "cjs") == 0) {
        return LANG_JS;
    }

    /* Rust */
    if (strcmp(ext, "rs") == 0) return LANG_RUST;

    /* Go */
    if (strcmp(ext, "go") == 0) return LANG_GO;

    /* JSON */
    if (strcmp(ext, "json") == 0) return LANG_JSON;

    /* Markdown */
    if (strcmp(ext, "md")       == 0 ||
        strcmp(ext, "markdown") == 0) {
        return LANG_MARKDOWN;
    }

    /* Shell */
    if (strcmp(ext, "sh")   == 0 || strcmp(ext, "bash") == 0 ||
        strcmp(ext, "zsh")  == 0 || strcmp(ext, "ksh")  == 0) {
        return LANG_SHELL;
    }

    /* Makefile fragment */
    if (strcmp(ext, "mk") == 0 || strcmp(ext, "make") == 0) {
        return LANG_MAKEFILE;
    }

    return LANG_NONE;
}

/* =============================================================================
 * syntax_highlight_line — public API
 *
 * Dispatches to the appropriate per-language tokenizer.
 * ============================================================================= */
int syntax_highlight_line(SyntaxLang lang,
                          const char *text, int len,
                          SyntaxToken *tokens,
                          int ml_state_in)
{
    /*
     * Guard against NULL inputs or lines longer than SYNTAX_MAX_LINE.
     * We clamp `len` rather than refusing to highlight, so very long lines
     * still get partially highlighted.
     */
    if (text == NULL || tokens == NULL || len <= 0) return ml_state_in;
    if (len > SYNTAX_MAX_LINE) len = SYNTAX_MAX_LINE;

    switch (lang) {
        case LANG_C:
            return highlight_generic(&CFG_C, text, len, tokens, ml_state_in);

        case LANG_PYTHON:
            return highlight_python(text, len, tokens, ml_state_in);

        case LANG_JS:
            return highlight_generic(&CFG_JS, text, len, tokens, ml_state_in);

        case LANG_RUST:
            return highlight_generic(&CFG_RUST, text, len, tokens, ml_state_in);

        case LANG_GO:
            return highlight_generic(&CFG_GO, text, len, tokens, ml_state_in);

        case LANG_JSON:
            return highlight_json(text, len, tokens, ml_state_in);

        case LANG_MARKDOWN:
            return highlight_markdown(text, len, tokens, ml_state_in);

        case LANG_SHELL:
            return highlight_generic(&CFG_SHELL, text, len, tokens, ml_state_in);

        case LANG_MAKEFILE:
            return highlight_makefile(text, len, tokens, ml_state_in);

        default:
            /* LANG_NONE or unknown: leave all tokens as SYN_NORMAL */
            memset(tokens, SYN_NORMAL, (size_t)len);
            return 0;
    }
}
