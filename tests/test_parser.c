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

/* Parsing src must fail with a message containing msg, at any position. */
static void check_error_any(const char *src, const char *msg) {
    Arena a;
    ParseError err;
    File *f;

    arena_init(&a);
    f = parse_file(src, &a, &err);
    if (f) {
        fprintf(stderr, "expected a parse error containing \"%s\"\n", msg);
        failures++;
    } else if (!strstr(err.msg, msg)) {
        fprintf(stderr, "wrong error: got \"%s\", want ...%s...\n", err.msg, msg);
        failures++;
    }
    arena_free(&a);
}

/* prefix + (unit repeated n times) + suffix, malloc'ed */
static char *build(const char *prefix, const char *unit, size_t n, const char *suffix) {
    size_t lp = strlen(prefix), lu = strlen(unit), ls = strlen(suffix);
    char *s = malloc(lp + lu * n + ls + 1);
    size_t i;

    if (!s) {
        exit(1);
    }
    memcpy(s, prefix, lp);
    for (i = 0; i < n; i++) {
        memcpy(s + lp + i * lu, unit, lu);
    }
    memcpy(s + lp + n * lu, suffix, ls + 1);
    return s;
}

static void test_limits(void) {
    char *s;

    /* moderately deep nesting is fine */
    s = build("func f() = ", "(", 100, "1");
    {
        char *t = build(s, ")", 100, "");
        Arena a;
        ParseError err;
        arena_init(&a);
        if (!parse_file(t, &a, &err)) {
            fprintf(stderr, "100 nested parens should parse: %s\n", err.msg);
            failures++;
        }
        arena_free(&a);
        free(t);
    }
    free(s);

    /* absurd nesting is an error, not a crash */
    s = build("func f() = ", "(", 100000, "1");
    check_error_any(s, "nesting is too deep");
    free(s);
    s = build("func f() = ", "-", 100000, "1");
    check_error_any(s, "nesting is too deep");
    free(s);
    s = build("func f() = ", "{", 100000, "");
    check_error_any(s, "nesting is too deep");
    free(s);
    s = build("struct S = { f: ", "*", 100000, "i32 }");
    check_error_any(s, "nesting is too deep");
    free(s);
    s = build("func f() = a", "+a", 100000, "");
    check_error_any(s, "expression is too long");
    free(s);
    s = build("func f() = a", ".b", 100000, "");
    check_error_any(s, "expression is too long");
    free(s);
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

static void test_blocks(void) {
    check_expr("{}", "(block)");
    check_expr("{ 1 }", "(block (tail 1))");
    check_expr("{ a; b }", "(block (expr a) (tail b))");
    check_expr("{ ; ; a }", "(block (tail a))");
    check_expr("{ { 1 } }", "(block (tail (block (tail 1))))");
    check("func f() = {\n  a\n  b\n}", "(func f () void (block (expr a) (tail b)))");
    check("func f() = {\n  a\n  b;\n}", "(func f () void (block (expr a) (expr b)))");
}

static void test_statements(void) {
    check("func f() = {\n  let x = 1\n  var y: i32 = 2\n  x + y\n}",
          "(func f () void (block (let x 1) (var y i32 2) (tail (+ x y))))");
    check_expr("{ let p: *u8 = f() }", "(block (let p (ptr u8) (call f)))");
    check_expr("{ defer a.free(p)\n 0 }", "(block (defer (call (. a free) p)) (tail 0))");
    check_expr("{ return }", "(block (return))");
    check_expr("{ return 1 }", "(block (return 1))");
    check_expr("{ return; 1 }", "(block (return) (tail 1))");
    check("func f() = {\n  return\n  1\n}", "(func f () void (block (return) (tail 1)))");
    check_expr("{ x = 1; x }", "(block (expr (= x 1)) (tail x))");
}

static void test_control_flow(void) {
    check_expr("if a { 1 } else { 2 }", "(if a (block (tail 1)) (block (tail 2)))");
    check_expr("if a {}", "(if a (block))");
    check_expr("if a { 1 } else if b { 2 } else { 3 }",
               "(if a (block (tail 1)) (if b (block (tail 2)) (block (tail 3))))");
    check_expr("if x < 0 { -x } else { x }", "(if (< x 0) (block (tail (- x))) (block (tail x)))");
    check_expr("if f(a, b) { 1 }", "(if (call f a b) (block (tail 1)))");
    check("func f() = if a {\n  1\n}\nelse {\n  2\n}",
          "(func f () void (if a (block (tail 1)) (block (tail 2))))");
    check_expr("while i < n { i = i + 1 }", "(while (< i n) (block (tail (= i (+ i 1)))))");
    check_expr("{ let v = if a { 1 } else { 2 }\n v }",
               "(block (let v (if a (block (tail 1)) (block (tail 2)))) (tail v))");
    check_expr("{ if a { f() }\n g() }",
               "(block (expr (if a (block (tail (call f))))) (tail (call g)))");
    check_expr("{ while a { b }\n while c { d } }",
               "(block (expr (while a (block (tail b)))) (tail (while c (block (tail d)))))");
}

static void test_struct_literals(void) {
    check_expr("Point { x: 1, y: 2 }", "(lit Point (x 1) (y 2))");
    check_expr("std.geo.Point { x: 1 }", "(lit std.geo.Point (x 1))");
    check_expr("Point {}", "(lit Point)");
    check("func f() = P {\n  x: 1\n  y: 2,\n}", "(func f () void (lit P (x 1) (y 2)))");
    check_expr("Line { a: P { x: 1 }, b: g(P {}) }", "(lit Line (a (lit P (x 1))) (b (call g (lit P))))");
    check_expr("P { x: 1 }.x", "(. (lit P (x 1)) x)");
    check_expr("{ let p = P { x: 1 }\n p }", "(block (let p (lit P (x 1))) (tail p))");
    check_expr("{ return P { x: 1 } }", "(block (return (lit P (x 1))))");
    /* in an if/while condition `Name {` starts the body; parentheses bring literals back */
    check_expr("if a { b }", "(if a (block (tail b)))");
    check_expr("if (P { x: 1 }) == q { 1 }", "(if (== (lit P (x 1)) q) (block (tail 1)))");
    check_expr("if f(P { x: 1 }) { 1 }", "(if (call f (lit P (x 1))) (block (tail 1)))");
    check_expr("while a { P { x: 1 } }", "(while a (block (tail (lit P (x 1)))))");
    /* a call result is not a path, so `{` after it is not a literal (and a second
     * statement on the same line needs ';') */
    check_error("func f() = { f() { 1 } }", 1, 18, "expected ';' or a newline");
    check_expr("{ f(); { 1 } }", "(block (expr (call f)) (tail (block (tail 1))))");
}

static void test_hello_world(void) {
    check("unfold std.console\n\nexport func main() -> void = {\n"
          "    std.console.println(\"Hello, World!\")\n}\n",
          "(unfold std.console)\n"
          "(export func main () void (block (tail (call (. (. std console) println) "
          "\"Hello, World!\"))))");
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
    check_error("func f() = { a b }", 1, 16, "expected ';' or a newline");
    check_error("func f() = { a", 1, 15, "expected '}'");
    check_error("func f() = { let = 1 }", 1, 18, "expected a variable name");
    check_error("func f() = { let x 1 }", 1, 20, "expected '='");
    check_error("func f() = { return 1 2 }", 1, 23, "expected ';' or a newline");
    check_error("func f() = if a b", 1, 17, "expected '{', found 'b'");
    check_error("func f() = if a {} else 1", 1, 25, "expected '{', found '1'");
    check_error("func f() = else", 1, 12, "expected an expression, found 'else'");
    check_error("func f() = while a", 1, 19, "expected '{', found end of file");
    check_error("func f() = P { x 1 }", 1, 14, "expected ',' or a newline");
    check_error("func f() = P { x: }", 1, 19, "expected an expression, found '}'");
    check_error("func f() = P { x: 1 y: 2 }", 1, 21, "expected ',' or a newline");
    check_error("func f() = P { x: 1", 1, 20, "expected '}'");
    check_error("func f() = a.", 1, 14, "expected a field name");
    check_error("func f() = a(1 2)", 1, 16, "expected ')'");
    check_error("func f() = a[1", 1, 15, "expected ']'");
    check_error("func f() -> i32!= 1", 1, 16, "write 'T! =' with a space");
}

int main(void) {
    test_limits();
    test_unfold();
    test_struct();
    test_types();
    test_func();
    test_postfix();
    test_operators();
    test_unary_cast_assign();
    test_operator_lines();
    test_blocks();
    test_statements();
    test_control_flow();
    test_struct_literals();
    test_hello_world();
    test_errors();

    if (failures) {
        fprintf(stderr, "%d test(s) failed\n", failures);
        return 1;
    }
    printf("parser: all tests passed\n");
    return 0;
}
