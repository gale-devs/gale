#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "arena.h"
#include "ast.h"
#include "parser.h"
#include "sema.h"

static int failures = 0;

/* Parses and checks src. Returns 1 if both succeed. On a semantic error fills *err;
 * a parse error is reported as a test failure. */
static int run(const char *src, SemaError *err) {
    Arena a;
    ParseError pe;
    File *f;
    Sema *s;
    int ok;

    arena_init(&a);
    f = parse_file(src, &a, &pe);
    if (!f) {
        fprintf(stderr, "test source does not parse:\n%s\n  %d:%d: %s\n", src, pe.line, pe.col, pe.msg);
        failures++;
        arena_free(&a);
        return 0;
    }
    s = sema_new(&a);
    ok = sema_check(s, f, err);
    sema_free(s);
    arena_free(&a);
    return ok;
}

static void check_ok(const char *src) {
    SemaError err;
    if (!run(src, &err) && err.msg[0]) {
        fprintf(stderr, "expected no error for:\n%s\n  got %d:%d: %s\n", src, err.line, err.col, err.msg);
        failures++;
    }
}

static void check_err(const char *src, int line, int col, const char *msg) {
    SemaError err;
    memset(&err, 0, sizeof err);
    if (run(src, &err)) {
        fprintf(stderr, "expected an error for:\n%s\n", src);
        failures++;
    } else if (err.msg[0] && (err.line != line || err.col != col || !strstr(err.msg, msg))) {
        fprintf(stderr, "wrong error for:\n%s\n  got:  %d:%d: %s\n  want: %d:%d: ...%s...\n", src,
                err.line, err.col, err.msg, line, col, msg);
        failures++;
    }
}

/* Type of the body of function f (with a fixed set of parameters), as text, or
 * "ERROR line:col: message". */
#define PRELUDE "func f(x: i32, y: u8, b: bool, d: f64, p: *u8) = "

static void expr_type(const char *expr, char *out, size_t cap) {
    char src[400];
    Arena a;
    ParseError pe;
    SemaError err;
    File *f;
    Sema *s;

    snprintf(src, sizeof src, PRELUDE "%s", expr);
    arena_init(&a);
    f = parse_file(src, &a, &pe);
    if (!f) {
        snprintf(out, cap, "PARSE ERROR %d:%d: %s", pe.line, pe.col, pe.msg);
    } else {
        s = sema_new(&a);
        if (sema_check(s, f, &err)) {
            snprintf(out, cap, "%s", ty_str(sema_types(s), f->body.items[0]->u.func.body->ty));
        } else {
            snprintf(out, cap, "ERROR %d:%d: %s", err.line, err.col, err.msg);
        }
        sema_free(s);
    }
    arena_free(&a);
}

static void check_type(const char *expr, const char *want) {
    char got[400];
    expr_type(expr, got, sizeof got);
    if (strcmp(got, want) != 0) {
        fprintf(stderr, "type of `%s`:\n  got:  %s\n  want: %s\n", expr, got, want);
        failures++;
    }
}

/* The expression must fail at `col` (1-based, within the expression) with a message
 * containing msg. */
static void check_expr_err(const char *expr, int col, const char *msg) {
    char got[400], want_pos[40];

    expr_type(expr, got, sizeof got);
    snprintf(want_pos, sizeof want_pos, "ERROR 1:%d: ", (int)strlen(PRELUDE) + col);
    if (strncmp(got, want_pos, strlen(want_pos)) != 0 || !strstr(got, msg)) {
        fprintf(stderr, "error for `%s`:\n  got:  %s\n  want: %s...%s...\n", expr, got, want_pos, msg);
        failures++;
    }
}

static void test_literals_and_names(void) {
    check_type("1", "i32");
    check_type("1.5", "f64");
    check_type("\"hi\"", "[]u8");
    check_type("true", "bool");
    check_type("x", "i32");
    check_type("p", "*u8");
    check_expr_err("nope", 1, "unknown name 'nope'");
}

static void test_arithmetic(void) {
    check_type("x + 1", "i32");
    check_type("y + 1", "u8");
    check_type("1 + y", "u8");
    check_type("x + 1 * 2", "i32");
    check_type("d * 2.0", "f64");
    check_type("d + 1", "f64"); /* an integer literal can be a float */
    check_type("x % 3", "i32");
    check_type("y & 0x0F | 1", "u8");
    check_type("x ^ 1", "i32");
    check_type("x << 2", "i32");
    check_type("y << 3", "u8");
    check_type("x << y", "i32");
    check_expr_err("x + y", 1, "mismatched types for '+': i32 and u8");
    check_expr_err("x + d", 1, "mismatched types for '+': i32 and f64");
    check_expr_err("b + 1", 1, "mismatched types for '+': bool and i32");
    check_expr_err("b + b", 1, "'+' needs numeric operands, found bool");
    check_expr_err("d % 2.0", 1, "'%' needs integer operands, found f64");
    check_expr_err("d << 1", 1, "'<<' needs integer operands");
}

static void test_literal_ranges(void) {
    check_type("y + 255", "u8");
    check_type("y + 0xFF", "u8");
    check_type("y + 0b1_0", "u8");
    check_expr_err("y + 256", 5, "the value does not fit in u8");
    check_expr_err("x + 2147483648", 5, "the value does not fit in i32");
    check_type("x + -2147483648", "i32");
    check_expr_err("x + -2147483649", 6, "the value does not fit in i32");
    check_expr_err("y + -1", 6, "the value does not fit in u8");
    check_type("x as i64 + 9223372036854775807", "i64");
    check_type("x as i64 + -9223372036854775808", "i64");
    check_expr_err("x as i64 + 9223372036854775808", 12, "the value does not fit in i64");
    check_type("x as u64 + 18446744073709551615", "u64");
    check_expr_err("x as u64 + 18446744073709551616", 12, "the integer literal is too large");
    check_type("-1", "i32");
    check_expr_err("2147483648", 1, "the value does not fit in i32");
}

static void test_logic_and_comparison(void) {
    check_type("x == 1", "bool");
    check_type("x < 2 && b", "bool");
    check_type("!b || b", "bool");
    check_type("b == b", "bool");
    check_type("p == p", "bool");
    check_type("d >= 1.5", "bool");
    check_type("1 < x", "bool");
    check_expr_err("x == y", 1, "cannot compare i32 with u8");
    check_expr_err("b < b", 1, "'<' does not work on bool");
    check_expr_err("x && b", 1, "'&&' needs bool operands, found i32 and bool");
    check_expr_err("!x", 1, "'!' needs a bool, found i32");
}

static void test_unary(void) {
    check_type("-x", "i32");
    check_type("-d", "f64");
    check_type("~y", "u8");
    check_type("- -x", "i32");
    check_expr_err("-y", 1, "cannot negate the unsigned type u8");
    check_expr_err("-b", 1, "cannot negate bool");
    check_expr_err("~d", 1, "'~' needs an integer, found f64");
}

static void test_casts(void) {
    check_type("x as u8", "u8");
    check_type("y as f64", "f64");
    check_type("d as i32", "i32");
    check_type("p as usize", "usize");
    check_type("x as usize as *u8", "*u8");
    check_type("p as *i32", "*i32");
    check_type("b as i32", "i32");
    check_type("1 as f32", "f32");
    check_type("-x as i64", "i64");
    check_expr_err("d as bool", 1, "cannot cast f64 to bool");
    check_expr_err("x as void", 6, "a cast target cannot have type void");
    check_expr_err("x as Foo", 6, "unknown type 'Foo'");
    check_expr_err("\"hi\" as i32", 1, "cannot cast []u8 to i32");
}

/* Declarations shared by the tests below. */
#define PRE                                                                   \
    "struct Point = { x: i32, y: i32 }\n"                                      \
    "func add(a: i32, b: i32) -> i32 = a + b\n"                                \
    "struct Counter = {\n"                                                     \
    "  n: i32\n"                                                               \
    "  export func get(c: *Counter) -> i32 = c.n\n"                                   \
    "  export func peek(c: Counter) -> i32 = c.n\n"                                   \
    "  export func plus(c: *Counter, k: i32) -> i32 = c.n + k\n"                      \
    "  export func bad(k: i32) -> i32 = k\n"                                          \
    "}\n"                                                                      \
    "func nothing() = {}\n"                                                    \
    "struct lib = {\n"                                                         \
    "  export struct math = {\n"                                               \
    "    export func twice(a: i32) -> i32 = a + a\n"                           \
    "    func hidden() -> i32 = 0\n"                                           \
    "  }\n"                                                                    \
    "  struct inner = { func f() -> i32 = 1 }\n"                               \
    "}\n"

static int pre_lines(void) {
    int n = 0;
    const char *c;
    for (c = PRE; *c; c++) {
        if (*c == '\n') {
            n++;
        }
    }
    return n;
}

/* A function `t` with the given signature, e.g. "(p: Point) -> i32", whose body is
 * `expr` on its own line, indented by two spaces. */
static void build_t(char *out, size_t cap, const char *sig, const char *expr) {
    snprintf(out, cap, PRE "func t%s =\n  %s", sig, expr);
}

static void check_t_ok(const char *sig, const char *expr) {
    char src[1200];
    build_t(src, sizeof src, sig, expr);
    check_ok(src);
}

/* `expr` must fail at `col` (1-based, within the expression) with msg. */
static void check_t_err(const char *sig, const char *expr, int col, const char *msg) {
    char src[1200];
    build_t(src, sizeof src, sig, expr);
    check_err(src, pre_lines() + 2, 2 + col, msg);
}

/* The error must be reported at the first occurrence of `needle` inside the test
 * function, so tests name the offending token instead of counting columns. */
static void check_t_err_at(const char *sig, const char *expr, const char *needle, const char *msg) {
    char src[1600];
    const char *t, *at, *c;
    int line = 1, col = 1;

    build_t(src, sizeof src, sig, expr);
    t = strstr(src, "\nfunc t(");
    at = t ? strstr(t + 1, needle) : NULL;
    if (!at) {
        fprintf(stderr, "test bug: `%s` not found in:\n%s\n", needle, src);
        failures++;
        return;
    }
    for (c = src; c < at; c++) {
        if (*c == '\n') {
            line++;
            col = 1;
        } else {
            col++;
        }
    }
    check_err(src, line, col, msg);
}

static void test_calls(void) {
    check_t_ok("() -> i32", "add(1, 2)");
    check_t_ok("() -> i32", "add(add(1, 2), 3) + add(4, 5)");
    check_t_ok("(a: u8) -> u8", "a + 1");
    check_t_err("() -> i32", "add(1)", 1, "'add' takes 2 arguments, found 1");
    check_t_err("() -> i32", "add(1, 2, 3)", 1, "'add' takes 2 arguments, found 3");
    check_t_err("() -> i32", "add(1, true)", 8, "argument 2 of 'add': expected i32, found bool");
    check_t_err("(a: i64) -> i32", "add(a, 1)", 5, "argument 1 of 'add': expected i32, found i64");
    check_t_err("() -> i32", "add(1, 3000000000)", 8, "the value does not fit in i32");
    check_t_err("(x: i32) -> i32", "x(1)", 1, "'x' is a variable, not a function");
    check_t_err("() -> i32", "Point(1)", 1, "'Point' is a type, not a function");
    check_t_err("() -> i32", "nope(1)", 1, "unknown name 'nope'");
    check_t_err("() -> i32", "(1)(2)", 2, "this expression is not a function");
    check_t_err("() -> i32", "add", 1, "'add' is a function; call it with ()");
    check_t_err("() -> i32", "Point", 1, "'Point' is a type, not a value");
    /* the result type is the declared return type */
    check_t_err("() -> bool", "add(1, 2)", 1, "the body has type i32, but the function returns bool");
}

static void test_paths_and_visibility(void) {
    check_t_ok("() -> i32", "lib.math.twice(2)");
    /* `inner` is not exported, so it is private to `lib` even from the same file */
    check_t_err("() -> i32", "lib.inner.f()", 1, "'inner' is private to 'lib'");
    check_t_err("() -> i32", "lib.math.hidden()", 1, "'hidden' is private to 'math'");
    check_t_err("() -> i32", "lib.math.nope()", 1, "'math' has no member 'nope'");
    check_t_err("() -> i32", "lib.nope.f()", 1, "'lib' has no member 'nope'");
    check_t_err("() -> i32", "lib.math.twice.x", 1, "'twice' is a function and has no members");
    /* a private member is fine from inside its struct, and from nested structs */
    check_ok("struct a = {\n  func p() -> i32 = 1\n  struct b = { func q() -> i32 = p() }\n}");
    check_err("struct a = { func p() -> i32 = 1 }\nfunc q() -> i32 = a.p()", 2, 19,
              "'p' is private to 'a'");
    check_ok("struct a = { export func p() -> i32 = 1 }\nfunc q() -> i32 = a.p()");
}

static void test_fields_and_literals(void) {
    check_t_ok("(p: Point) -> i32", "p.x * p.x + p.y * p.y");
    check_t_ok("(p: *Point) -> i32", "p.x"); /* a pointer is dereferenced automatically */
    check_t_ok("() -> Point", "Point { x: 3, y: 4 }");
    check_t_ok("() -> i32", "Point { x: 3, y: 4 }.y");
    check_t_ok("(a: u8) -> Point", "Point { y: 1, x: 2 }");
    check_t_err("(p: Point) -> i32", "p.z", 1, "Point has no field 'z'");
    check_t_err("(a: i32) -> i32", "a.x", 1, "i32 has no fields");
    check_t_err("(c: Counter) -> i32", "c.get", 1, "'get' is not a field; call it with ()");
    check_t_err("() -> Point", "Point { x: 1 }", 1, "missing field 'y'");
    check_t_err("() -> Point", "Point { x: 1, y: 2, z: 3 }", 21, "Point has no field 'z'");
    check_t_err("() -> Point", "Point { x: 1, x: 2, y: 3 }", 15, "field 'x' is given twice");
    check_t_err("() -> Point", "Point { x: 1, y: true }", 18, "expected i32, found bool");
    check_t_err("() -> Point", "Nope { x: 1 }", 1, "unknown type 'Nope'");
    check_t_err("() -> Point", "add { x: 1 }", 1, "'add' is not a type");
    check_t_err("() -> i32", "Point { x: 1, y: 2 }", 1, "the body has type Point, but the function returns i32");
}

static void test_methods(void) {
    check_t_ok("(c: Counter) -> i32", "c.get()");                 /* takes the address of c */
    check_t_ok("(c: *Counter) -> i32", "c.peek()");               /* dereferences c */
    check_t_ok("(c: *Counter) -> i32", "c.get()");
    check_t_ok("(c: Counter) -> i32", "c.peek()");
    check_t_ok("(c: Counter) -> i32", "c.plus(5)");
    check_t_err("(c: Counter) -> i32", "c.plus()", 1, "'plus' takes 1 argument, found 0");
    check_t_err("(c: Counter) -> i32", "c.plus(1, 2)", 1, "'plus' takes 1 argument, found 2");
    check_t_err("(c: Counter) -> i32", "c.plus(true)", 8, "argument 1 of 'plus': expected i32, found bool");
    check_t_err("(c: Counter) -> i32", "c.bad()", 1,
                "the first parameter of 'bad' is i32, which does not take a receiver of type Counter");
    check_t_err("(c: Counter) -> i32", "c.n()", 1, "Counter has no method 'n'");
    check_t_err("() -> i32", "Counter { n: 1 }.get()", 1, "cannot take the address of this receiver");
    check_t_ok("() -> i32", "Counter { n: 1 }.peek()");
    check_t_err("(a: i32) -> i32", "a.get()", 1, "i32 has no methods");
}

static void test_index_and_pointers(void) {
    check_t_ok("(a: []u8, i: usize) -> u8", "a[i]");
    check_t_ok("(a: [4]i32) -> i32", "a[0]");
    check_t_ok("(p: *u8) -> u8", "p[1]");
    check_t_ok("(a: []u8, i: i32) -> u8", "a[i]");
    check_t_ok("(a: []Point) -> i32", "a[0].x");
    check_t_err("(a: []u8) -> u8", "a[true]", 3, "an index must be an integer, found bool");
    check_t_err("(a: i32) -> i32", "a[0]", 1, "cannot index i32");
    check_t_ok("(p: *i32) -> i32", "*p");
    check_t_ok("(x: i32) -> *i32", "&x");
    check_t_ok("(p: Point) -> *i32", "&p.x");
    check_t_ok("(p: *Point) -> *i32", "&p.y");
    check_t_ok("(a: []u8) -> *u8", "&a[0]");
    check_t_ok("(p: **i32) -> i32", "**p");
    check_t_err("(x: i32) -> i32", "*x", 1, "cannot dereference i32");
    check_t_err("() -> *i32", "&1", 1, "cannot take the address of this expression");
    check_t_err("() -> *i32", "&add(1, 2)", 1, "cannot take the address of this expression");
    check_t_err("() -> *i32", "&Point { x: 1, y: 2 }.x", 1, "cannot take the address of this expression");
    check_t_err("(x: i32) -> *i64", "&x", 1, "the body has type *i32, but the function returns *i64");
}

static void test_variables_and_scopes(void) {
    check_t_ok("() -> i32", "{\n  let a = 1\n  let b = a + 2\n  b\n}");
    check_t_ok("() -> i32", "{\n  var n = 0\n  n = n + 1\n  n\n}");
    check_t_ok("() -> i32", "{\n  let a: u8 = 200\n  a as i32\n}");
    check_t_ok("() -> i32", "{\n  let p = Point { x: 1, y: 2 }\n  p.x + p.y\n}");
    check_t_ok("() -> i32", "{\n  let v = if true { 1 } else { 2 }\n  v\n}");
    check_t_err_at("() -> i32", "{\n  let a: u8 = 300\n  0\n}", "300", "the value does not fit in u8");
    check_t_err_at("() -> i32", "{\n  let a: i64 = add(1, 2)\n  0\n}", "add(1, 2)", "expected i64, found i32");
    check_t_err_at("() -> i32", "{\n  let a = nothing()\n  0\n}", "nothing()", "no value to store");
    check_t_err_at("() -> i32", "{\n  let a: void = nothing()\n  0\n}", "void", "a variable cannot have type void");
    /* names cannot be reused while in scope */
    check_t_err_at("() -> i32", "{\n  let a = 1\n  let a = 2\n  a\n}", "let a = 2", "'a' is already declared");
    check_t_err_at("(x: i32) -> i32", "{\n  let x = 1\n  x\n}", "let x", "'x' is already declared");
    check_t_err_at("() -> i32", "{\n  let a = 1\n  { let a = 2 }\n  a\n}", "let a = 2", "'a' is already declared");
    /* a variable ends with its block */
    check_t_err_at("() -> i32", "{\n  { let a = 1 }\n  a\n}", "a\n}", "unknown name 'a'");
    check_t_ok("() -> i32", "{\n  { let a = 1 }\n  let a = 2\n  a\n}");
    check_t_err_at("() -> i32", "{\n  let a = a\n  a\n}", "a\n  a", "unknown name 'a'");
}

static void test_assignment(void) {
    check_t_ok("() -> i32", "{\n  var n = 1\n  n = 2\n  n\n}");
    check_t_ok("(p: *Point) -> i32", "{\n  p.x = 5\n  p.x\n}");
    check_t_ok("() -> i32", "{\n  var p = Point { x: 1, y: 2 }\n  p.x = 3\n  p.x\n}");
    check_t_ok("(a: []u8) -> u8", "{\n  a[0] = 1\n  a[0]\n}");
    check_t_ok("(p: *i32) -> i32", "{\n  *p = 3\n  *p\n}");
    check_t_ok("() -> u8", "{\n  var b: u8 = 1\n  b = 255\n  b\n}");
    check_t_err_at("() -> i32", "{\n  let n = 1\n  n = 2\n  n\n}", "n = 2",
                   "cannot assign to 'n': it is declared with let (use var)");
    check_t_err_at("(x: i32) -> i32", "{\n  x = 2\n  x\n}", "x = 2", "parameters are immutable");
    check_t_err_at("(p: Point) -> i32", "{\n  p.x = 5\n  p.x\n}", "p.x = 5", "not mutable");
    check_t_err_at("() -> i32", "{\n  let p = Point { x: 1, y: 2 }\n  p.x = 3\n  0\n}", "p.x = 3", "not mutable");
    check_t_err_at("(a: [4]u8) -> u8", "{\n  a[0] = 1\n  0\n}", "a[0] = 1", "not mutable");
    check_t_err_at("() -> i32", "{\n  1 = 2\n  0\n}", "1 = 2", "cannot assign to this expression");
    check_t_err_at("() -> i32", "{\n  add(1, 2) = 3\n  0\n}", "add(1, 2) = 3", "cannot assign to this expression");
    check_t_err_at("() -> i32", "{\n  var n = 1\n  n = true\n  n\n}", "true", "expected i32, found bool");
    check_t_err_at("() -> u8", "{\n  var b: u8 = 1\n  b = 256\n  b\n}", "256", "the value does not fit in u8");
    /* assignment is a statement-like expression of type void: no chaining */
    check_t_err_at("() -> i32", "{\n  var n = 1\n  var m = 2\n  n = m = 1\n  n\n}", "m = 1", "expected i32, found void");
}

static void test_return(void) {
    check_t_ok("() -> i32", "{ return 1 }");
    check_t_ok("(x: i32) -> i32", "{\n  if x < 0 { return 0 }\n  x\n}");
    check_t_ok("(x: i32) -> i32", "{\n  if x < 0 { return 0 } else { return 1 }\n}");
    check_t_ok("() -> void", "{ return }");
    check_t_ok("() -> i64", "{ return 1 }");
    check_t_ok("() -> i32!", "{ return 1 }");
    check_t_err_at("() -> i32", "{ return }", "return", "must return a value of type i32");
    check_t_err_at("() -> void", "{ return 1 }", "return", "does not return a value");
    check_t_err_at("() -> i32", "{ return true }", "true", "expected i32, found bool");
    check_t_err_at("() -> u8", "{ return 300 }", "300", "the value does not fit in u8");
    /* falling off the end of a function that returns a value */
    check_t_err_at("(x: i32) -> i32", "{\n  if x < 0 { return 0 }\n}", "{\n  if", "the body has type void, but the function returns i32");
    check_t_err_at("() -> i32", "{}", "{}", "the body has type void, but the function returns i32");
}

static void test_if_and_while(void) {
    check_t_ok("(b: bool) -> i32", "if b { 1 } else { 2 }");
    check_t_ok("(x: i32) -> i32", "if x < 0 { -1 } else if x == 0 { 0 } else { 1 }");
    check_t_ok("(b: bool) -> u8", "if b { 1 } else { 255 }");
    check_t_ok("(b: bool, v: u8) -> u8", "if b { v } else { 0 }");
    check_t_ok("(b: bool) -> i32", "if b { return 1 } else { 2 }");
    check_t_ok("(b: bool) -> void", "if b { nothing() }");
    check_t_ok("(b: bool) -> i32!", "if b { 1 } else { 2 }");
    check_t_err_at("(b: bool) -> i32", "if b { 1 }", "if", "the body has type void, but the function returns i32");
    check_t_err_at("(b: bool) -> i32", "if b { 1 } else { true }", "{ true }", "if and else have different types: i32 and bool");
    check_t_err_at("(x: i32) -> i32", "if x { 1 } else { 2 }", "x {", "expected bool, found i32");
    check_t_err_at("(b: bool) -> u8", "if b { 1 } else { 256 }", "256", "the value does not fit in u8");
    check_t_ok("() -> i32", "{\n  var i = 0\n  while i < 10 { i = i + 1 }\n  i\n}");
    check_t_ok("() -> void", "while true { nothing() }");
    check_t_err_at("() -> i32", "{\n  while 1 { }\n  0\n}", "1 {", "expected bool, found i32");
    check_t_err_at("() -> i32", "while true { }", "while", "the body has type void, but the function returns i32");
}

static void test_defer_and_try(void) {
    check_t_ok("() -> i32", "{\n  defer nothing()\n  1\n}");
    check_t_ok("() -> i32", "{\n  add(1, 2)\n  3\n}"); /* a statement may discard a value */
    check_t_ok("(e: i32!) -> i32!", "{\n  let v = try e\n  v + 1\n}");
    check_t_ok("(e: i32!) -> i32!", "try e");
    check_t_err_at("(e: i32!) -> i32", "try e", "try", "'try' can only be used in a function that returns an error union");
    check_t_err_at("(x: i32) -> i32!", "try x", "try", "'try' needs an error union, found i32");
}

static void test_function_bodies(void) {
    check_ok("func f(a: i32) -> i32 = a + 1");
    check_ok("func f(a: u8) -> u8 = a + 255");
    check_ok("func f(a: i32!) -> i32! = a");
    check_ok("func f() -> i32! = 1"); /* T is accepted where T! is expected */
    check_ok("func f() -> ?i32 = 1");
    check_ok("func f() = 1 + 2"); /* a void function discards its body's value */
    check_err("func f() -> bool = 1", 1, 20, "the body has type i32, but the function returns bool");
    check_err("func f(a: i32) -> i64 = a", 1, 25, "the body has type i32, but the function returns i64");
    check_err("func f() -> u8 = 300", 1, 18, "the value does not fit in u8");
    check_ok("func f() -> u8 = 255");
    check_ok("func f() -> f32 = 1");
    check_err("func f(a: i32, b: i32) -> i32 = a + c", 1, 37, "unknown name 'c'");
}

static void test_declarations(void) {
    check_ok("");
    check_ok("struct P = { x: i32, y: i32 }\nfunc f(p: *P) -> i32 = 0");
    check_ok("struct A = {\n  struct B = { v: i32 }\n  b: B\n}");
    check_ok("struct A = { export struct B = { v: i32 } }\nstruct C = { b: A.B }");
    check_ok("struct S = { a: [0x10]u8, b: []u8, c: ?*S, d: [2][3]f64 }");
    check_ok("func f(x: i32!) -> i32! = x");
    check_ok("func f(a: i8, b: u16, c: isize, d: usize, e: f32, g: bool) -> u64 = 0");
}

static void test_type_errors(void) {
    check_err("struct S = { x: Foo }", 1, 17, "unknown type 'Foo'");
    check_err("func g() = 0\nstruct S = { x: g }", 2, 17, "'g' is not a type");
    check_err("struct A = {}\nstruct S = { x: A.B }", 2, 17, "'A' has no member 'B'");
    check_err("func f() = 0\nstruct S = { x: f.y }", 2, 17, "'f' is not a struct, so it has no member 'y'");
    check_err("struct S = { x: void }", 1, 17, "a field cannot have type void");
    check_err("func f(a: void) = 0", 1, 11, "a parameter cannot have type void");
    check_err("struct S = { x: *void }", 1, 18, "a pointer target cannot have type void");
    check_err("struct S = { a: [N]i32 }", 1, 18, "an array length must be an integer literal");
    check_err("struct S = { a: [99999999999999999999]u8 }", 1, 18, "array length is too large");
}

static void test_visibility(void) {
    check_err("struct A = { struct B = { v: i32 } }\nstruct C = { b: A.B }", 2, 17,
              "'B' is private to 'A'");
    /* inside the struct, private members are fine */
    check_ok("struct A = {\n  struct B = { v: i32 }\n  struct C = { b: B }\n}");
    check_ok("struct A = {\n  struct B = { v: i32 }\n  func f(b: A.B) = 0\n}");
}

static void test_duplicates(void) {
    check_err("struct S = { x: i32, x: i32 }", 1, 22, "field 'x' is declared twice");
    check_err("func f() = 0\nfunc f() = 1", 2, 1, "'f' is already defined");
    check_err("struct S = {}\nfunc S() = 1", 2, 1, "'S' is already defined");
    check_err("struct S = {\n  x: i32\n  func x() = 0\n}", 3, 3, "'x' is both a field and a member");
    check_err("func f(a: i32, a: i32) = 0", 1, 16, "parameter 'a' is declared twice");
    check_err("unfold a.b", 1, 1, "'unfold' must be expanded");
}

static void test_recursion(void) {
    check_ok("struct N = { next: *N }");
    check_ok("struct N = { kids: []N }");
    check_err("struct N = { next: N }", 1, 1, "struct 'N' contains itself");
    check_err("struct N = { a: [2]N }", 1, 1, "struct 'N' contains itself");
    check_err("struct N = { a: ?N }", 1, 1, "struct 'N' contains itself");
    check_err("struct A = { b: B }\nstruct B = { a: A }", 1, 1, "contains itself");
}

int main(void) {
    test_literals_and_names();
    test_arithmetic();
    test_literal_ranges();
    test_logic_and_comparison();
    test_unary();
    test_casts();
    test_calls();
    test_paths_and_visibility();
    test_fields_and_literals();
    test_methods();
    test_index_and_pointers();
    test_variables_and_scopes();
    test_assignment();
    test_return();
    test_if_and_while();
    test_defer_and_try();
    test_function_bodies();
    test_declarations();
    test_type_errors();
    test_visibility();
    test_duplicates();
    test_recursion();

    if (failures) {
        fprintf(stderr, "%d test(s) failed\n", failures);
        return 1;
    }
    printf("sema: all tests passed\n");
    return 0;
}
