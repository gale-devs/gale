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
