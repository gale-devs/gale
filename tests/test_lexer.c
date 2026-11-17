#include <stdio.h>
#include <string.h>

#include "lexer.h"

static int failures = 0;

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            failures++;                                                          \
        }                                                                        \
    } while (0)

/* The token stream of src must be exactly kinds[0..n-1] followed by EOF. */
static void expect_kinds(const char *src, const TokenKind *kinds, unsigned long n) {
    Lexer lx;
    unsigned long i;
    Token t;

    lexer_init(&lx, src);
    for (i = 0; i < n; i++) {
        t = lexer_next(&lx);
        if (t.kind != kinds[i]) {
            fprintf(stderr, "kind mismatch at token %lu of \"%s\": got %s, want %s\n", i, src,
                    token_kind_name(t.kind), token_kind_name(kinds[i]));
            failures++;
            return;
        }
    }
    t = lexer_next(&lx);
    if (t.kind != TOK_EOF) {
        fprintf(stderr, "expected EOF after %lu tokens of \"%s\", got %s\n", n, src,
                token_kind_name(t.kind));
        failures++;
    }
}

#define EXPECT(src, ...)                                                         \
    do {                                                                         \
        static const TokenKind k_[] = {__VA_ARGS__};                             \
        expect_kinds((src), k_, (unsigned long)(sizeof(k_) / sizeof(k_[0])));    \
    } while (0)

static int text_is(Token t, const char *s) {
    return t.len == strlen(s) && memcmp(t.start, s, t.len) == 0;
}

static void test_hello(void) {
    EXPECT("unfold std.console\n"
           "export func main() -> void = {\n"
           "    std.console.println(\"Hello, World!\")\n"
           "}\n",
           TOK_KW_UNFOLD, TOK_IDENT, TOK_DOT, TOK_IDENT,
           TOK_KW_EXPORT, TOK_KW_FUNC, TOK_IDENT, TOK_LPAREN, TOK_RPAREN, TOK_ARROW,
           TOK_IDENT, TOK_ASSIGN, TOK_LBRACE,
           TOK_IDENT, TOK_DOT, TOK_IDENT, TOK_DOT, TOK_IDENT, TOK_LPAREN, TOK_STRING, TOK_RPAREN,
           TOK_RBRACE);
}

static void test_operators(void) {
    EXPECT("-> == != <= >= << >> && || = < > + - * / % & | ^ ~ ! ?",
           TOK_ARROW, TOK_EQ, TOK_NE, TOK_LE, TOK_GE, TOK_SHL, TOK_SHR, TOK_ANDAND, TOK_OROR,
           TOK_ASSIGN, TOK_LT, TOK_GT, TOK_PLUS, TOK_MINUS, TOK_STAR, TOK_SLASH, TOK_PERCENT,
           TOK_AMP, TOK_PIPE, TOK_CARET, TOK_TILDE, TOK_BANG, TOK_QUESTION);
    EXPECT("( ) { } [ ] , : ; .",
           TOK_LPAREN, TOK_RPAREN, TOK_LBRACE, TOK_RBRACE, TOK_LBRACKET, TOK_RBRACKET,
           TOK_COMMA, TOK_COLON, TOK_SEMI, TOK_DOT);
}

static void test_keywords(void) {
    EXPECT("unfolded unfold let letx", TOK_IDENT, TOK_KW_UNFOLD, TOK_KW_LET, TOK_IDENT);
    EXPECT("export func struct let var if else while defer return try as true false",
           TOK_KW_EXPORT, TOK_KW_FUNC, TOK_KW_STRUCT, TOK_KW_LET, TOK_KW_VAR, TOK_KW_IF,
           TOK_KW_ELSE, TOK_KW_WHILE, TOK_KW_DEFER, TOK_KW_RETURN, TOK_KW_TRY, TOK_KW_AS,
           TOK_KW_TRUE, TOK_KW_FALSE);
}

static void test_numbers(void) {
    Lexer lx;
    Token t;

    EXPECT("0xFF 0b101 1_000 3.14 1e10 2.5e-3 42",
           TOK_INT, TOK_INT, TOK_INT, TOK_FLOAT, TOK_FLOAT, TOK_FLOAT, TOK_INT);

    lexer_init(&lx, "0xFF 1_000 2.5e-3");
    t = lexer_next(&lx);
    CHECK(text_is(t, "0xFF"));
    t = lexer_next(&lx);
    CHECK(text_is(t, "1_000"));
    t = lexer_next(&lx);
    CHECK(text_is(t, "2.5e-3"));

    /* "1." is an int followed by a dot, not a float */
    EXPECT("1.x", TOK_INT, TOK_DOT, TOK_IDENT);

    EXPECT("12abc", TOK_ERROR);
    EXPECT("0x", TOK_ERROR);
    EXPECT("0b12", TOK_ERROR);
    EXPECT("1e", TOK_ERROR);
}

static void test_strings(void) {
    Lexer lx;
    Token t;

    EXPECT("\"a\\nb\" \"\\\"\"", TOK_STRING, TOK_STRING);

    lexer_init(&lx, "\"abc");
    t = lexer_next(&lx);
    CHECK(t.kind == TOK_ERROR);
    CHECK(t.err != NULL && strcmp(t.err, "unterminated string literal") == 0);

    lexer_init(&lx, "\"a\nb\"");
    t = lexer_next(&lx);
    CHECK(t.kind == TOK_ERROR);

    lexer_init(&lx, "\"\\q\"");
    t = lexer_next(&lx);
    CHECK(t.kind == TOK_ERROR);
    CHECK(t.err != NULL && strcmp(t.err, "invalid escape sequence") == 0);
}

static void test_comments_and_newlines(void) {
    Lexer lx;
    Token a, b, c;

    lexer_init(&lx, "a // line comment\n/* block\ncomment */ b c");
    a = lexer_next(&lx);
    b = lexer_next(&lx);
    c = lexer_next(&lx);
    CHECK(a.kind == TOK_IDENT && !a.nl_before);
    CHECK(b.kind == TOK_IDENT && b.nl_before);
    CHECK(c.kind == TOK_IDENT && !c.nl_before);
    CHECK(lexer_next(&lx).kind == TOK_EOF);

    EXPECT("/* abc", TOK_ERROR);
}

static void test_positions(void) {
    Lexer lx;
    Token t;

    lexer_init(&lx, "ab\n  cd");
    t = lexer_next(&lx);
    CHECK(t.line == 1 && t.col == 1);
    t = lexer_next(&lx);
    CHECK(t.line == 2 && t.col == 3);
    CHECK(text_is(t, "cd"));
}

static void test_errors(void) {
    EXPECT("@", TOK_ERROR);
    /* a multi-byte UTF-8 character outside strings and comments is one error */
    EXPECT("\xc3\xa9", TOK_ERROR);
    /* UTF-8 inside strings and comments is fine */
    EXPECT("\"\xe3\x81\x82\" // \xe3\x81\x82", TOK_STRING);
}

static void test_eof_is_sticky(void) {
    Lexer lx;

    lexer_init(&lx, "");
    CHECK(lexer_next(&lx).kind == TOK_EOF);
    CHECK(lexer_next(&lx).kind == TOK_EOF);
}

int main(void) {
    test_hello();
    test_operators();
    test_keywords();
    test_numbers();
    test_strings();
    test_comments_and_newlines();
    test_positions();
    test_errors();
    test_eof_is_sticky();

    if (failures) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    printf("lexer: all tests passed\n");
    return 0;
}
