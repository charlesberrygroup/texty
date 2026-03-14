/*
 * syntax.h — Syntax Highlighting API
 * =============================================================================
 * This module converts lines of source text into per-character "token types"
 * (keyword, string, comment, etc.).  It has NO dependency on ncurses or any
 * terminal library — it is pure logic that can be tested independently.
 *
 * The display module (display.c) calls syntax_highlight_line() for each
 * visible line, then maps the returned token types to ncurses color pairs.
 * This keeps all terminal code in one place and all language rules here.
 *
 * Supported languages (detected by file extension):
 *   C/C++, Python, JavaScript/TypeScript, Rust, Go, JSON, Markdown, Shell,
 *   Makefile.
 * =============================================================================
 */

#ifndef SYNTAX_H
#define SYNTAX_H

#include <stdint.h>   /* uint8_t */

/* =============================================================================
 * SyntaxLang — identifies which programming language a buffer contains.
 *
 * syntax_detect_language() maps a filename to one of these values.
 * LANG_NONE means "plain text" and produces no highlighting.
 * ============================================================================= */
typedef enum {
    LANG_NONE     = 0,  /* plain text — no highlighting                      */
    LANG_C,             /* C or C++  (.c .h .cpp .cc .cxx .hpp)              */
    LANG_PYTHON,        /* Python    (.py)                                    */
    LANG_JS,            /* JavaScript / TypeScript (.js .ts .jsx .tsx .mjs)  */
    LANG_RUST,          /* Rust      (.rs)                                    */
    LANG_GO,            /* Go        (.go)                                    */
    LANG_JSON,          /* JSON      (.json)                                  */
    LANG_MARKDOWN,      /* Markdown  (.md .markdown)                         */
    LANG_SHELL,         /* Shell     (.sh .bash .zsh)                        */
    LANG_MAKEFILE,      /* Makefile  (Makefile makefile GNUmakefile *.mk)    */
    LANG_COUNT          /* sentinel — total number of languages              */
} SyntaxLang;

/* =============================================================================
 * SyntaxToken — the "kind" of a single character, used to choose a color.
 *
 * We use uint8_t (one byte per character) rather than int (four bytes) so the
 * per-line token array stays small.  All values fit in a byte since there are
 * only seven token types.
 * ============================================================================= */
typedef uint8_t SyntaxToken;

#define SYN_NORMAL   ((SyntaxToken)0)   /* ordinary text — terminal default  */
#define SYN_KEYWORD  ((SyntaxToken)1)   /* language keyword (if, for, ...)   */
#define SYN_TYPE     ((SyntaxToken)2)   /* built-in type (int, void, ...)    */
#define SYN_STRING   ((SyntaxToken)3)   /* string / character literal        */
#define SYN_COMMENT  ((SyntaxToken)4)   /* comment (line or block)           */
#define SYN_PREPROC  ((SyntaxToken)5)   /* preprocessor directive (#include) */
#define SYN_NUMBER   ((SyntaxToken)6)   /* numeric literal (42, 0xFF, 3.14)  */
#define SYN_COUNT    7                  /* total number of token types       */

/*
 * SYNTAX_MAX_LINE — maximum characters highlighted per line.
 *
 * Characters beyond this position are left as SYN_NORMAL.  1024 comfortably
 * covers almost all real source code lines (PEP 8 recommends 79, Google style
 * 100, etc.).  At 1 byte per token, this array is only 1 KB on the stack.
 */
#define SYNTAX_MAX_LINE 1024

/* =============================================================================
 * Public API
 * ============================================================================= */

/*
 * syntax_detect_language — infer the language from a filename.
 *
 * Looks at the file extension, or the base name for files like "Makefile".
 * Returns LANG_NONE if the extension is unknown or if filename is NULL.
 *
 * Examples:
 *   syntax_detect_language("src/editor.c")  → LANG_C
 *   syntax_detect_language("Makefile")       → LANG_MAKEFILE
 *   syntax_detect_language(NULL)             → LANG_NONE
 */
SyntaxLang syntax_detect_language(const char *filename);

/*
 * syntax_highlight_line — tokenize one line of source text.
 *
 * Parameters:
 *   lang        — which language to use.  LANG_NONE sets all tokens SYN_NORMAL.
 *   text        — pointer to the line's characters.  Need NOT be NUL-terminated.
 *   len         — number of characters (not counting any trailing newline).
 *   tokens      — caller-allocated array of at least `len` SyntaxToken values.
 *                 On return, tokens[i] holds the token type for text[i].
 *   ml_state_in — multi-line parse state entering this line:
 *                   0 = normal (not inside any multi-line construct)
 *                   1 = inside a C-style block comment (slash-star...star-slash)
 *                   2 = inside a Python triple-double-quoted string  """
 *                   3 = inside a Python triple-single-quoted string  '''
 *
 * Returns the multi-line state to pass to the NEXT line's call.
 *
 * Typical usage (render loop):
 *
 *   int state = 0;
 *   for (int r = 0; r < buf->num_lines; r++) {
 *       SyntaxToken tokens[SYNTAX_MAX_LINE];
 *       state = syntax_highlight_line(lang,
 *                   buffer_get_line(buf, r),
 *                   buffer_line_len(buf, r),
 *                   tokens, state);
 *       // tokens[0..len-1] now hold colors for each character on row r
 *   }
 */
int syntax_highlight_line(SyntaxLang lang,
                          const char *text, int len,
                          SyntaxToken *tokens,
                          int ml_state_in);

#endif /* SYNTAX_H */
