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

static void test_declarations(void) {
    check_ok("");
    check_ok("struct P = { x: i32, y: i32 }\nfunc f(p: *P) -> i32 = 0");
    check_ok("struct A = {\n  struct B = { v: i32 }\n  b: B\n}");
    check_ok("struct A = { export struct B = { v: i32 } }\nstruct C = { b: A.B }");
    check_ok("struct S = { a: [0x10]u8, b: []u8, c: ?*S, d: [2][3]f64 }");
    check_ok("func f() -> void! = 0");
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
