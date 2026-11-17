#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "arena.h"
#include "ast.h"
#include "parser.h"

static int failures = 0;

/* Parses src and compares the S-expression dump with want. */
static void check(const char *src, const char *want) {
    Arena a;
    ParseError err;
    File *f;
    char *got;

    arena_init(&a);
    f = parse_file(src, &a, &err);
    if (!f) {
        fprintf(stderr, "parse failed for:\n%s\n  at %d:%d: %s\n", src, err.line, err.col, err.msg);
        failures++;
        arena_free(&a);
        return;
    }
    got = ast_dump(f);
    if (strcmp(got, want) != 0) {
        fprintf(stderr, "dump mismatch for:\n%s\n  got:  %s\n  want: %s\n", src, got, want);
        failures++;
    }
    free(got);
    arena_free(&a);
}

/* Parsing src must fail at line:col with a message containing msg. */
static void check_error(const char *src, int line, int col, const char *msg) {
    Arena a;
    ParseError err;
    File *f;

    arena_init(&a);
    f = parse_file(src, &a, &err);
    if (f) {
        fprintf(stderr, "expected a parse error for:\n%s\n", src);
        failures++;
    } else if (err.line != line || err.col != col || !strstr(err.msg, msg)) {
        fprintf(stderr, "wrong error for:\n%s\n  got:  %d:%d: %s\n  want: %d:%d: ...%s...\n", src,
                err.line, err.col, err.msg, line, col, msg);
        failures++;
    }
    arena_free(&a);
}

static void test_unfold(void) {
    check("", "");
    check("unfold std.console\n", "(unfold std.console)");
    check("export unfold a.b", "(export unfold a.b)");
    check("unfold a\nunfold b.c.d\n", "(unfold a)\n(unfold b.c.d)");
}

static void test_struct(void) {
    check("struct E = {}", "(struct E)");
    check("struct Point = { x: f64, y: f64 }", "(struct Point (x f64) (y f64))");
    check("export struct P = {\n  x: i32\n  y: i32,\n}\n", "(export struct P (x i32) (y i32))");
    check("struct Outer = {\n  struct Inner = { v: i32 }\n  w: i32\n}\n",
          "(struct Outer (w i32) (struct Inner (v i32)))");
}

static void test_types(void) {
    check("struct T = { a: *i32, b: ?*u8, c: []u8, d: [4]i32, e: i32!, f: std.mem.Alloc }",
          "(struct T (a (ptr i32)) (b (opt (ptr u8))) (c (slice u8)) (d (array 4 i32)) "
          "(e (err i32)) (f std.mem.Alloc))");
    check("struct T = { a: *[]u8, b: [2][3]i32, c: ?i32! }",
          "(struct T (a (ptr (slice u8))) (b (array 2 (array 3 i32))) (c (err (opt i32))))");
}

static void test_func(void) {
    check("func a() = 1", "(func a () void 1)");
    check("export func f(x: i32, p: *u8,) -> i32! = 2.5",
          "(export func f ((x i32) (p (ptr u8))) (err i32) 2.5)");
    check("func g(s: []u8) -> bool = (true)", "(func g ((s (slice u8))) bool true)");
    check("func h() = \"hi\"\nfunc k() = false", "(func h () void \"hi\")\n(func k () void false)");
    check("struct S = { func m(x: i32) -> i32 = x\n n: i32 }",
          "(struct S (n i32) (func m ((x i32)) i32 x))");
}

static void test_postfix(void) {
    check("func f() = a.b.c(1, x)[2]", "(func f () void (index (call (. (. a b) c) 1 x) 2))");
    check("func f() = g()", "(func f () void (call g))");
    check("func f() = g(1,)", "(func f () void (call g 1))");
    check("func f() = g(\n  1,\n  2,\n)", "(func f () void (call g 1 2))");
    check("func f() = a[b[0]](c)", "(func f () void (call (index a (index b 0)) c))");
    check("func f() = (a.b)(1)", "(func f () void (call (. a b) 1))");
    check("func f() = x.\n  y", "(func f () void (. x y))");
    /* a line starting with '(' is not a call of the previous line */
    check_error("func f() = a\n(b)", 2, 1, "expected an item");
}

/* Wraps an expression in a function so it can be checked on its own. */
static void check_expr(const char *expr, const char *want) {
    char src[256];
    char full[300];

    snprintf(src, sizeof src, "func f() = %s", expr);
    snprintf(full, sizeof full, "(func f () void %s)", want);
    check(src, full);
}

static void test_operators(void) {
    check_expr("1 + 2 * 3", "(+ 1 (* 2 3))");
    check_expr("1 - 2 - 3", "(- (- 1 2) 3)");
    check_expr("(1 + 2) * 3", "(* (+ 1 2) 3)");
    check_expr("a || b && c == d", "(|| a (&& b (== c d)))");
    check_expr("a | b ^ c & d << 1 + 2", "(| a (^ b (& c (<< d (+ 1 2)))))");
    check_expr("a << 2 >= b", "(>= (<< a 2) b)");
    check_expr("a % b / c", "(/ (% a b) c)");
    check_expr("a != b", "(!= a b)");
    check_expr("a < b", "(< a b)");
}

static void test_unary_cast_assign(void) {
    check_expr("-a * b", "(* (- a) b)");
    check_expr("- -x", "(- (- x))");
    check_expr("!a && b", "(&& (! a) b)");
    check_expr("~a", "(~ a)");
    check_expr("*p.x", "(deref (. p x))");
    check_expr("&x", "(addr x)");
    check_expr("try f(x)", "(try (call f x))");
    check_expr("f(a & b, &c)", "(call f (& a b) (addr c))");
    check_expr("a as i32 * 2", "(* (as a i32) 2)");
    check_expr("-a as u8", "(as (- a) u8)");
    check_expr("p as *u8", "(as p (ptr u8))");
    check_expr("a as i32 as i64", "(as (as a i32) i64)");
    check_expr("a = b = c", "(= a (= b c))");
    check_expr("x.y = 1 + 2", "(= (. x y) (+ 1 2))");
    check_expr("a[i] = *p", "(= (index a i) (deref p))");
}

static void test_operator_lines(void) {
    /* a line may end with an operator ... */
    check("func f() = a +\n  b", "(func f () void (+ a b))");
    check("func f() = a =\n  b", "(func f () void (= a b))");
    /* ... but a line starting with one begins a new item */
    check_error("func f() = a\n+ b", 2, 1, "expected an item");
    check_error("func f() = a\n= b", 2, 1, "expected an item");
    check_error("func f() = 1 +", 1, 15, "expected an expression, found end of file");
}

static void test_errors(void) {
    check_error("struct = {}", 1, 8, "expected a struct name");
    check_error("x: i32", 1, 1, "fields are not allowed at file level");
    check_error("unfold", 1, 7, "expected a name, found end of file");
    check_error("unfold a.b @", 1, 12, "unexpected character");
    check_error("struct S = { x: i32", 1, 20, "expected '}'");
    check_error("struct S = { x: i32 y: i32 }", 1, 21, "expected ',' or a newline");
    check_error("struct S = { x: }", 1, 17, "expected a type");
    check_error("struct S = { 1 }", 1, 14, "expected a field or an item");
    check_error("func = 1", 1, 6, "expected a function name");
    check_error("func f( = 1", 1, 9, "expected a parameter name");
    check_error("func f() 1", 1, 10, "expected '='");
    check_error("func f() =", 1, 11, "expected an expression, found end of file");
    check_error("func f() = )", 1, 12, "expected an expression, found ')'");
    check_error("func f() = a.", 1, 14, "expected a field name");
    check_error("func f() = a(1 2)", 1, 16, "expected ')'");
    check_error("func f() = a[1", 1, 15, "expected ']'");
    check_error("func f() -> i32!= 1", 1, 16, "write 'T! =' with a space");
}

int main(void) {
    test_unfold();
    test_struct();
    test_types();
    test_func();
    test_postfix();
    test_operators();
    test_unary_cast_assign();
    test_operator_lines();
    test_errors();

    if (failures) {
        fprintf(stderr, "%d test(s) failed\n", failures);
        return 1;
    }
    printf("parser: all tests passed\n");
    return 0;
}
