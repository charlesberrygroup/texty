/*
 * test_syntax.c — Unit tests for src/syntax.c
 * =============================================================================
 * Tests cover:
 *   1. syntax_detect_language() — correct language from filename/extension
 *   2. syntax_highlight_line() — per-language token classification:
 *        - C:      keywords, types, strings, line/block comments, preprocessor,
 *                  numbers, multi-line block comment state
 *        - Python: triple-quoted strings (multi-line state), hash comments
 *        - JSON:   strings, numbers, keywords (true/false/null)
 *        - Shell:  hash comments, keyword detection
 *        - LANG_NONE: everything stays SYN_NORMAL
 * =============================================================================
 */

#include "test_runner.h"
#include "syntax.h"

#include <string.h>  /* strlen */

/* =============================================================================
 * Test helpers
 * ============================================================================= */

/*
 * tokenize — convenience wrapper: highlight one C-string line with the given
 * language, writing results into `out`.  Returns the ml_state for the next line.
 */
static int tokenize(SyntaxLang lang, const char *line,
                    SyntaxToken *out, int ml_state_in)
{
    int len = (int)strlen(line);
    return syntax_highlight_line(lang, line, len, out, ml_state_in);
}

/*
 * all_eq — return 1 if every element in tokens[0..len-1] equals `expected`.
 */
static int all_eq(const SyntaxToken *tokens, int len, SyntaxToken expected)
{
    for (int i = 0; i < len; i++) {
        if (tokens[i] != expected) return 0;
    }
    return 1;
}

/* =============================================================================
 * 1. syntax_detect_language
 * ============================================================================= */

static void test_detect_c(void)
{
    ASSERT(syntax_detect_language("main.c")       == LANG_C, "main.c → LANG_C");
    ASSERT(syntax_detect_language("editor.h")     == LANG_C, "editor.h → LANG_C");
    ASSERT(syntax_detect_language("app.cpp")      == LANG_C, "app.cpp → LANG_C");
    ASSERT(syntax_detect_language("util.cc")      == LANG_C, "util.cc → LANG_C");
    ASSERT(syntax_detect_language("widget.cxx")   == LANG_C, "widget.cxx → LANG_C");
    ASSERT(syntax_detect_language("foo.hpp")      == LANG_C, "foo.hpp → LANG_C");
    ASSERT(syntax_detect_language("src/editor.c") == LANG_C, "path → LANG_C");
}

static void test_detect_python(void)
{
    ASSERT(syntax_detect_language("script.py")  == LANG_PYTHON, ".py → LANG_PYTHON");
    ASSERT(syntax_detect_language("app.pyw")    == LANG_PYTHON, ".pyw → LANG_PYTHON");
}

static void test_detect_js(void)
{
    ASSERT(syntax_detect_language("app.js")     == LANG_JS, ".js → LANG_JS");
    ASSERT(syntax_detect_language("types.ts")   == LANG_JS, ".ts → LANG_JS");
    ASSERT(syntax_detect_language("comp.jsx")   == LANG_JS, ".jsx → LANG_JS");
    ASSERT(syntax_detect_language("comp.tsx")   == LANG_JS, ".tsx → LANG_JS");
    ASSERT(syntax_detect_language("module.mjs") == LANG_JS, ".mjs → LANG_JS");
}

static void test_detect_rust(void)
{
    ASSERT(syntax_detect_language("main.rs") == LANG_RUST, ".rs → LANG_RUST");
}

static void test_detect_go(void)
{
    ASSERT(syntax_detect_language("main.go") == LANG_GO, ".go → LANG_GO");
}

static void test_detect_json(void)
{
    ASSERT(syntax_detect_language("config.json") == LANG_JSON, ".json → LANG_JSON");
}

static void test_detect_markdown(void)
{
    ASSERT(syntax_detect_language("README.md")      == LANG_MARKDOWN, ".md");
    ASSERT(syntax_detect_language("notes.markdown") == LANG_MARKDOWN, ".markdown");
}

static void test_detect_shell(void)
{
    ASSERT(syntax_detect_language("build.sh")   == LANG_SHELL, ".sh → LANG_SHELL");
    ASSERT(syntax_detect_language("setup.bash") == LANG_SHELL, ".bash");
    ASSERT(syntax_detect_language("rc.zsh")     == LANG_SHELL, ".zsh");
}

static void test_detect_makefile(void)
{
    ASSERT(syntax_detect_language("Makefile")    == LANG_MAKEFILE, "Makefile");
    ASSERT(syntax_detect_language("makefile")    == LANG_MAKEFILE, "makefile");
    ASSERT(syntax_detect_language("GNUmakefile") == LANG_MAKEFILE, "GNUmakefile");
    ASSERT(syntax_detect_language("rules.mk")    == LANG_MAKEFILE, ".mk");
}

static void test_detect_none(void)
{
    ASSERT(syntax_detect_language(NULL)         == LANG_NONE, "NULL → LANG_NONE");
    ASSERT(syntax_detect_language("README")     == LANG_NONE, "no ext → LANG_NONE");
    ASSERT(syntax_detect_language("foo.txt")    == LANG_NONE, ".txt → LANG_NONE");
    ASSERT(syntax_detect_language("foo.xyz")    == LANG_NONE, ".xyz → LANG_NONE");
    ASSERT(syntax_detect_language(".gitignore") == LANG_NONE, ".gitignore → LANG_NONE");
}

/* =============================================================================
 * 2. C / C++ highlighting
 * ============================================================================= */

static void test_c_keywords(void)
{
    SyntaxToken tok[SYNTAX_MAX_LINE];

    tokenize(LANG_C, "if (x > 0)", tok, 0);
    ASSERT(tok[0] == SYN_KEYWORD, "if[0] == KEYWORD");
    ASSERT(tok[1] == SYN_KEYWORD, "if[1] == KEYWORD");
    ASSERT(tok[2] == SYN_NORMAL,  "space after if == NORMAL");

    tokenize(LANG_C, "return 0;", tok, 0);
    ASSERT(tok[0] == SYN_KEYWORD, "return[0] == KEYWORD");
    ASSERT(tok[5] == SYN_KEYWORD, "return[5] == KEYWORD");
    ASSERT(tok[6] == SYN_NORMAL,  "space after return == NORMAL");

    tokenize(LANG_C, "for (int i = 0;", tok, 0);
    ASSERT(tok[0] == SYN_KEYWORD, "for[0] == KEYWORD");
    ASSERT(tok[2] == SYN_KEYWORD, "for[2] == KEYWORD");
}

static void test_c_types(void)
{
    SyntaxToken tok[SYNTAX_MAX_LINE];

    tokenize(LANG_C, "int x = 0;", tok, 0);
    ASSERT(tok[0] == SYN_TYPE,   "int[0] == TYPE");
    ASSERT(tok[2] == SYN_TYPE,   "int[2] == TYPE");
    ASSERT(tok[3] == SYN_NORMAL, "space after int == NORMAL");

    tokenize(LANG_C, "void foo(void)", tok, 0);
    ASSERT(tok[0] == SYN_TYPE, "void == TYPE");

    tokenize(LANG_C, "uint8_t *p;", tok, 0);
    ASSERT(tok[0] == SYN_TYPE, "uint8_t == TYPE");
}

static void test_c_string(void)
{
    SyntaxToken tok[SYNTAX_MAX_LINE];
    const char *line = "\"hello world\"";
    int len = (int)strlen(line);
    tokenize(LANG_C, line, tok, 0);
    ASSERT(all_eq(tok, len, SYN_STRING), "\"hello world\" all STRING");
}

static void test_c_string_escape(void)
{
    SyntaxToken tok[SYNTAX_MAX_LINE];
    /* \" inside the string should NOT end the string */
    tokenize(LANG_C, "\"say \\\"hi\\\"\"", tok, 0);
    ASSERT(tok[0] == SYN_STRING, "opening quote == STRING");
    ASSERT(tok[5] == SYN_STRING, "backslash == STRING");
    ASSERT(tok[6] == SYN_STRING, "escaped quote == STRING");
}

static void test_c_char_literal(void)
{
    SyntaxToken tok[SYNTAX_MAX_LINE];
    const char *line = "'x'";
    int len = (int)strlen(line);
    tokenize(LANG_C, line, tok, 0);
    ASSERT(all_eq(tok, len, SYN_STRING), "'x' all STRING");
}

static void test_c_line_comment(void)
{
    SyntaxToken tok[SYNTAX_MAX_LINE];
    const char *line = "// this is a comment";
    int len = (int)strlen(line);
    tokenize(LANG_C, line, tok, 0);
    ASSERT(all_eq(tok, len, SYN_COMMENT), "// line all COMMENT");
}

static void test_c_line_comment_after_code(void)
{
    SyntaxToken tok[SYNTAX_MAX_LINE];
    tokenize(LANG_C, "int x; // comment", tok, 0);
    ASSERT(tok[0] == SYN_TYPE,    "int before comment == TYPE");
    ASSERT(tok[7] == SYN_COMMENT, "// starts comment");
    ASSERT(tok[16] == SYN_COMMENT, "end of inline comment == COMMENT");
}

static void test_c_block_comment_single_line(void)
{
    SyntaxToken tok[SYNTAX_MAX_LINE];
    const char *line = "/* hello */";
    int len = (int)strlen(line);
    tokenize(LANG_C, line, tok, 0);
    ASSERT(all_eq(tok, len, SYN_COMMENT), "/* hello */ all COMMENT");
}

static void test_c_block_comment_open(void)
{
    SyntaxToken tok[SYNTAX_MAX_LINE];
    const char *line = "/* unclosed";
    int len = (int)strlen(line);
    int ml = tokenize(LANG_C, line, tok, 0);
    ASSERT(ml == 1, "unclosed block comment → ml_state 1");
    ASSERT(all_eq(tok, len, SYN_COMMENT), "unclosed block comment all COMMENT");
}

static void test_c_block_comment_continue(void)
{
    SyntaxToken tok[SYNTAX_MAX_LINE];

    /* Line 1: open */
    int ml = tokenize(LANG_C, "/* open", tok, 0);
    ASSERT(ml == 1, "open line → ml_state 1");

    /* Line 2: middle — still in comment */
    const char *mid = "still in comment";
    ml = tokenize(LANG_C, mid, tok, ml);
    ASSERT(ml == 1, "middle line → ml_state 1");
    ASSERT(all_eq(tok, (int)strlen(mid), SYN_COMMENT), "middle line all COMMENT");

    /* Line 3: close */
    const char *close_line = "end */";
    ml = tokenize(LANG_C, close_line, tok, ml);
    ASSERT(ml == 0, "close line → ml_state 0");
    ASSERT(all_eq(tok, (int)strlen(close_line), SYN_COMMENT), "close line all COMMENT");
}

static void test_c_preproc(void)
{
    SyntaxToken tok[SYNTAX_MAX_LINE];

    tokenize(LANG_C, "#include <stdio.h>", tok, 0);
    ASSERT(tok[0] == SYN_PREPROC, "# == PREPROC");
    ASSERT(tok[1] == SYN_PREPROC, "i of include == PREPROC");

    tokenize(LANG_C, "#define MAX 100", tok, 0);
    ASSERT(tok[0] == SYN_PREPROC, "#define # == PREPROC");
    ASSERT(tok[7] == SYN_PREPROC, "#define word == PREPROC");
}

static void test_c_number(void)
{
    SyntaxToken tok[SYNTAX_MAX_LINE];

    tokenize(LANG_C, "x = 42;", tok, 0);
    ASSERT(tok[4] == SYN_NUMBER, "42 first digit == NUMBER");
    ASSERT(tok[5] == SYN_NUMBER, "42 second digit == NUMBER");
    ASSERT(tok[6] == SYN_NORMAL, "; after number == NORMAL");

    tokenize(LANG_C, "x = 0xFF;", tok, 0);
    ASSERT(tok[4] == SYN_NUMBER, "0xFF '0' == NUMBER");
    ASSERT(tok[7] == SYN_NUMBER, "0xFF 'F' == NUMBER");

    tokenize(LANG_C, "f = 3.14;", tok, 0);
    ASSERT(tok[4] == SYN_NUMBER, "3.14 '3' == NUMBER");
    ASSERT(tok[7] == SYN_NUMBER, "3.14 '4' == NUMBER");
}

static void test_c_normal_identifier(void)
{
    SyntaxToken tok[SYNTAX_MAX_LINE];
    tokenize(LANG_C, "foo(bar);", tok, 0);
    ASSERT(tok[0] == SYN_NORMAL, "foo is NORMAL (not keyword)");
    ASSERT(tok[2] == SYN_NORMAL, "foo last char is NORMAL");
}

/* =============================================================================
 * 3. Python highlighting
 * ============================================================================= */

static void test_py_hash_comment(void)
{
    SyntaxToken tok[SYNTAX_MAX_LINE];
    const char *line = "# this is a comment";
    int len = (int)strlen(line);
    tokenize(LANG_PYTHON, line, tok, 0);
    ASSERT(all_eq(tok, len, SYN_COMMENT), "Python # comment all COMMENT");
}

static void test_py_hash_comment_after_code(void)
{
    SyntaxToken tok[SYNTAX_MAX_LINE];
    tokenize(LANG_PYTHON, "x = 1  # inline", tok, 0);
    ASSERT(tok[0] == SYN_NORMAL,  "x before comment == NORMAL");
    ASSERT(tok[7] == SYN_COMMENT, "# starts comment");
}

static void test_py_keywords(void)
{
    SyntaxToken tok[SYNTAX_MAX_LINE];

    tokenize(LANG_PYTHON, "if x:", tok, 0);
    ASSERT(tok[0] == SYN_KEYWORD, "Python if[0] == KEYWORD");
    ASSERT(tok[1] == SYN_KEYWORD, "Python if[1] == KEYWORD");
    ASSERT(tok[2] == SYN_NORMAL,  "space after if == NORMAL");

    tokenize(LANG_PYTHON, "def foo():", tok, 0);
    ASSERT(tok[0] == SYN_KEYWORD, "Python def == KEYWORD");
}

static void test_py_triple_double_open(void)
{
    SyntaxToken tok[SYNTAX_MAX_LINE];
    const char *line = "\"\"\"hello";
    int ml = tokenize(LANG_PYTHON, line, tok, 0);
    ASSERT(ml == 2, "triple-\" open → ml_state 2");
    ASSERT(all_eq(tok, (int)strlen(line), SYN_STRING), "triple-\" open all STRING");
}

static void test_py_triple_double_close(void)
{
    SyntaxToken tok[SYNTAX_MAX_LINE];
    const char *line = "world\"\"\"";
    int ml = tokenize(LANG_PYTHON, line, tok, 2);
    ASSERT(ml == 0, "triple-\" close → ml_state 0");
    ASSERT(all_eq(tok, (int)strlen(line), SYN_STRING), "triple-\" close all STRING");
}

static void test_py_triple_single_open(void)
{
    SyntaxToken tok[SYNTAX_MAX_LINE];
    const char *line = "'''hello";
    int ml = tokenize(LANG_PYTHON, line, tok, 0);
    ASSERT(ml == 3, "triple-' open → ml_state 3");
    ASSERT(all_eq(tok, (int)strlen(line), SYN_STRING), "triple-' open all STRING");
}

static void test_py_triple_single_close(void)
{
    SyntaxToken tok[SYNTAX_MAX_LINE];
    const char *line = "world'''";
    int ml = tokenize(LANG_PYTHON, line, tok, 3);
    ASSERT(ml == 0, "triple-' close → ml_state 0");
    ASSERT(all_eq(tok, (int)strlen(line), SYN_STRING), "triple-' close all STRING");
}

/* =============================================================================
 * 4. JSON highlighting
 * ============================================================================= */

static void test_json_string(void)
{
    SyntaxToken tok[SYNTAX_MAX_LINE];
    tokenize(LANG_JSON, "\"key\": \"value\"", tok, 0);
    ASSERT(tok[0] == SYN_STRING, "\"key\" opening quote == STRING");
    ASSERT(tok[4] == SYN_STRING, "\"key\" closing quote == STRING");
    ASSERT(tok[5] == SYN_NORMAL, ": after key == NORMAL");
    ASSERT(tok[7] == SYN_STRING, "\"value\" opening quote == STRING");
}

static void test_json_keywords(void)
{
    SyntaxToken tok[SYNTAX_MAX_LINE];

    tokenize(LANG_JSON, "true", tok, 0);
    ASSERT(tok[0] == SYN_KEYWORD, "JSON true == KEYWORD");

    tokenize(LANG_JSON, "false", tok, 0);
    ASSERT(tok[0] == SYN_KEYWORD, "JSON false == KEYWORD");

    tokenize(LANG_JSON, "null", tok, 0);
    ASSERT(tok[0] == SYN_KEYWORD, "JSON null == KEYWORD");
}

static void test_json_number(void)
{
    SyntaxToken tok[SYNTAX_MAX_LINE];

    tokenize(LANG_JSON, "42", tok, 0);
    ASSERT(tok[0] == SYN_NUMBER, "JSON 42[0] == NUMBER");
    ASSERT(tok[1] == SYN_NUMBER, "JSON 42[1] == NUMBER");

    tokenize(LANG_JSON, "-3.14", tok, 0);
    ASSERT(tok[0] == SYN_NUMBER, "JSON -3.14 '-' == NUMBER");
    ASSERT(tok[4] == SYN_NUMBER, "JSON -3.14 '4' == NUMBER");
}

/* =============================================================================
 * 5. Shell highlighting
 * ============================================================================= */

static void test_shell_comment(void)
{
    SyntaxToken tok[SYNTAX_MAX_LINE];
    const char *line = "# this is a shell comment";
    int len = (int)strlen(line);
    tokenize(LANG_SHELL, line, tok, 0);
    ASSERT(all_eq(tok, len, SYN_COMMENT), "Shell # comment all COMMENT");
}

static void test_shell_shebang(void)
{
    SyntaxToken tok[SYNTAX_MAX_LINE];
    /* #!/bin/sh — starts with #, so it's a comment */
    const char *line = "#!/bin/sh";
    int len = (int)strlen(line);
    tokenize(LANG_SHELL, line, tok, 0);
    ASSERT(all_eq(tok, len, SYN_COMMENT), "shebang all COMMENT");
}

static void test_shell_keyword(void)
{
    SyntaxToken tok[SYNTAX_MAX_LINE];

    tokenize(LANG_SHELL, "if [ -f foo ]; then", tok, 0);
    ASSERT(tok[0] == SYN_KEYWORD, "Shell if == KEYWORD");

    tokenize(LANG_SHELL, "then", tok, 0);
    ASSERT(tok[0] == SYN_KEYWORD, "Shell then == KEYWORD");
}

/* =============================================================================
 * 6. LANG_NONE — no highlighting
 * ============================================================================= */

static void test_lang_none(void)
{
    SyntaxToken tok[SYNTAX_MAX_LINE];
    const char *line = "int if return /* comment */";
    int len = (int)strlen(line);
    tokenize(LANG_NONE, line, tok, 0);
    ASSERT(all_eq(tok, len, SYN_NORMAL), "LANG_NONE: all NORMAL");
}

static void test_lang_none_null_input(void)
{
    SyntaxToken tok[SYNTAX_MAX_LINE];
    /* NULL text must not crash */
    int ml = syntax_highlight_line(LANG_C, NULL, 10, tok, 0);
    ASSERT(ml == 0, "NULL text: ml_state unchanged");

    /* Zero length must not crash */
    char buf[4] = "int";
    ml = syntax_highlight_line(LANG_C, buf, 0, tok, 0);
    ASSERT(ml == 0, "zero length: ml_state unchanged");
}

/* =============================================================================
 * main
 * ============================================================================= */

int main(void)
{
    printf("=== test_syntax ===\n");

    /* -- syntax_detect_language -- */
    RUN(test_detect_c);
    RUN(test_detect_python);
    RUN(test_detect_js);
    RUN(test_detect_rust);
    RUN(test_detect_go);
    RUN(test_detect_json);
    RUN(test_detect_markdown);
    RUN(test_detect_shell);
    RUN(test_detect_makefile);
    RUN(test_detect_none);

    /* -- C highlighting -- */
    RUN(test_c_keywords);
    RUN(test_c_types);
    RUN(test_c_string);
    RUN(test_c_string_escape);
    RUN(test_c_char_literal);
    RUN(test_c_line_comment);
    RUN(test_c_line_comment_after_code);
    RUN(test_c_block_comment_single_line);
    RUN(test_c_block_comment_open);
    RUN(test_c_block_comment_continue);
    RUN(test_c_preproc);
    RUN(test_c_number);
    RUN(test_c_normal_identifier);

    /* -- Python highlighting -- */
    RUN(test_py_hash_comment);
    RUN(test_py_hash_comment_after_code);
    RUN(test_py_keywords);
    RUN(test_py_triple_double_open);
    RUN(test_py_triple_double_close);
    RUN(test_py_triple_single_open);
    RUN(test_py_triple_single_close);

    /* -- JSON highlighting -- */
    RUN(test_json_string);
    RUN(test_json_keywords);
    RUN(test_json_number);

    /* -- Shell highlighting -- */
    RUN(test_shell_comment);
    RUN(test_shell_shebang);
    RUN(test_shell_keyword);

    /* -- LANG_NONE -- */
    RUN(test_lang_none);
    RUN(test_lang_none_null_input);

    TEST_SUMMARY();
}
