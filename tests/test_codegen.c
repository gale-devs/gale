#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__linux__) && defined(__x86_64__)
#include <sys/wait.h>
#define CAN_RUN 1
#else
#define CAN_RUN 0
#endif

#include "arena.h"
#include "asm.h"
#include "codegen.h"
#include "elf.h"
#include "encode.h"
#include "fsutil.h"
#include "parser.h"
#include "sema.h"

/* Compiles programs all the way to an ELF file and, on x86-64 Linux, runs them and
 * checks the exit status (the low 8 bits of main's result). Run from the repository
 * root: the programs are written to build/. */

#define PROG "build/cg_test_prog"

static int failures = 0;
static int ran = 0;

typedef struct {
    char msg[400];
    unsigned char *elf;
    size_t elf_len;
    char *asm_text;
} Build;

/* Parses, checks, generates, encodes. Returns 1 on success; otherwise b->msg says why. */
static int build(const char *src, Build *b) {
    Arena a;
    ParseError pe;
    SemaError se;
    CodegenError ce;
    File *f;
    Sema *s;
    Asm as;
    int entry;
    Code code;
    size_t *labels;
    char err[300];

    memset(b, 0, sizeof *b);
    arena_init(&a);
    f = parse_file(src, &a, &pe);
    if (!f) {
        snprintf(b->msg, sizeof b->msg, "parse %d:%d: %s", pe.line, pe.col, pe.msg);
        arena_free(&a);
        return 0;
    }
    s = sema_new(&a);
    if (!sema_check(s, f, &se)) {
        snprintf(b->msg, sizeof b->msg, "sema %d:%d: %s", se.line, se.col, se.msg);
        sema_free(s);
        arena_free(&a);
        return 0;
    }
    asm_init(&as, &a);
    if (!codegen(s, &as, &entry, &ce)) {
        snprintf(b->msg, sizeof b->msg, "codegen %d:%d: %s", ce.line, ce.col, ce.msg);
        sema_free(s);
        arena_free(&a);
        return 0;
    }
    b->asm_text = asm_format(&as);
    labels = arena_alloc(&a, (asm_label_count(&as) + 1) * sizeof(size_t));
    if (!asm_encode(&as, &a, &code, labels, err, sizeof err)) {
        snprintf(b->msg, sizeof b->msg, "encode: %s", err);
        sema_free(s);
        arena_free(&a);
        return 0;
    }
    b->elf = elf_build(code.data, code.len, labels[entry], &b->elf_len);
    sema_free(s);
    arena_free(&a);
    return b->elf != NULL;
}

static void release(Build *b) {
    free(b->elf);
    free(b->asm_text);
}

/* main's result, as the exit status */
static void run_case(const char *name, const char *src, int want) {
    Build b;

    if (!build(src, &b)) {
        fprintf(stderr, "%s: does not compile: %s\n", name, b.msg);
        failures++;
        release(&b);
        return;
    }
#if CAN_RUN
    {
        int status;
        if (!write_file(PROG, b.elf, b.elf_len, 1)) {
            fprintf(stderr, "%s: cannot write %s\n", name, PROG);
            failures++;
        } else {
            status = system("./" PROG);
            ran++;
            if (!WIFEXITED(status)) {
                fprintf(stderr, "%s: the program did not exit normally (status %d)\n", name, status);
                failures++;
            } else if (WEXITSTATUS(status) != (want & 255)) {
                fprintf(stderr, "%s: exit status %d, want %d\n  %s\n", name, WEXITSTATUS(status),
                        want & 255, src);
                failures++;
            }
        }
    }
#else
    (void)want;
#endif
    release(&b);
}

static void compile_error(const char *name, const char *src, const char *needle) {
    Build b;
    if (build(src, &b) || !strstr(b.msg, needle)) {
        fprintf(stderr, "%s: expected a compile error containing \"%s\", got \"%s\"\n", name, needle, b.msg);
        failures++;
    }
    release(&b);
}

#define MAIN(body) "func main() -> i32 = " body "\n"

static void test_arithmetic(void) {
    run_case("constant", MAIN("42"), 42);
    run_case("precedence", MAIN("2 + 3 * 4"), 14);
    run_case("parens", MAIN("(2 + 3) * 4"), 20);
    run_case("left assoc", MAIN("10 - 3 - 2"), 5);
    run_case("negate", MAIN("-5 + 10"), 5);
    run_case("div", MAIN("100 / 7"), 14);
    run_case("mod", MAIN("100 % 7"), 2);
    run_case("signed div", MAIN("-7 / 2"), -3);
    run_case("signed mod", MAIN("-7 % 3"), -1);
    run_case("neg divisor", MAIN("7 / -2"), -3);
    run_case("bitwise", MAIN("0xF0 & 0x3C | 1 ^ 2"), 51);
    run_case("not", MAIN("~5 & 0xFF"), 250);
    run_case("shl", MAIN("1 << 5"), 32);
    run_case("sar", MAIN("-16 >> 2"), -4);
    run_case("shr unsigned", MAIN("((255 as u8) >> 4) as i32"), 15);
    run_case("hex binary", MAIN("0x10 + 0b101 + 1_000"), 1021);
}

static void test_integer_widths(void) {
    run_case("u8 wraps", MAIN("{\n  let a: u8 = 250\n  (a + 10) as i32\n}"), 4);
    run_case("i8 wraps", MAIN("{\n  let a: i8 = 127\n  (a + 1) as i32\n}"), 128);
    run_case("i32 overflow", MAIN("{\n  let a: i32 = 2147483647\n  a + 1\n}"), 0);
    run_case("u16", MAIN("{\n  let a: u16 = 65535\n  (a + 2) as i32\n}"), 1);
    run_case("u32 big", MAIN("{\n  let a: u32 = 4294967295\n  (a >> 24) as i32\n}"), 255);
    run_case("u64 big", MAIN("{\n  let a: u64 = 18446744073709551615\n  (a >> 56) as i32\n}"), 255);
    run_case("i64", MAIN("{\n  let a: i64 = 3000000000\n  (a * 4 / 1000000000) as i32\n}"), 12);
    run_case("cast truncates", MAIN("(300 as u8) as i32"), 44);
    run_case("cast neg to u8", MAIN("(-1 as u8) as i32"), 255);
    run_case("cast u8 to i8", MAIN("(200 as i8) as i32"), 200);
    run_case("cast i8 sign-extends", MAIN("{\n  let a: i8 = -2\n  (a as i64) as i32 + 10\n}"), 8);
    run_case("mul wraps", MAIN("{\n  let a: u8 = 200\n  (a * 2) as i32\n}"), 144);
    run_case("neg wraps", MAIN("{\n  let a: i8 = -128\n  (-a) as i32\n}"), 128);
    run_case("u8 not", MAIN("{\n  let a: u8 = 5\n  (~a) as i32\n}"), 250);
    run_case("usize", MAIN("{\n  let a: usize = 7\n  (a * a) as i32\n}"), 49);
}

static void test_comparisons_and_logic(void) {
    run_case("lt", MAIN("if 3 < 4 { 1 } else { 0 }"), 1);
    run_case("ge", MAIN("if 3 >= 4 { 1 } else { 0 }"), 0);
    run_case("eq", MAIN("if 5 == 5 { 1 } else { 0 }"), 1);
    run_case("ne", MAIN("if 5 != 5 { 1 } else { 0 }"), 0);
    run_case("signed compare", MAIN("{\n  let a: i32 = -1\n  if a > 1 { 7 } else { 9 }\n}"), 9);
    run_case("unsigned compare", MAIN("{\n  let a: u32 = 4294967295\n  if a > 1 { 7 } else { 9 }\n}"), 7);
    run_case("u8 compare", MAIN("{\n  let a: u8 = 200\n  if a > 100 { 7 } else { 9 }\n}"), 7);
    run_case("and or", MAIN("if true && false || true { 1 } else { 0 }"), 1);
    run_case("not", MAIN("if !(1 < 2) { 1 } else { 0 }"), 0);
    run_case("bool value", MAIN("{\n  let b = 3 < 4\n  if b { 5 } else { 6 }\n}"), 5);
    run_case("bool and value", MAIN("{\n  let b = 3 < 4 && 5 < 6\n  b as i32\n}"), 1);
    run_case("bool or value", MAIN("{\n  let b = 3 > 4 || 5 > 6\n  b as i32\n}"), 0);
    run_case("nested logic", MAIN("{\n  let a = 5\n  if (a > 3 && a < 10) || a == 100 { 1 } else { 0 }\n}"), 1);
    run_case("else if", MAIN("{\n  let x = 5\n  if x < 0 { 1 } else if x == 0 { 2 } else if x < 10 { 3 } else { 4 }\n}"), 3);
    run_case("bool main", "func main() -> bool = 1 < 2\n", 1);
}

static void test_variables_and_loops(void) {
    run_case("let", MAIN("{\n  let a = 6\n  let b = 7\n  a * b\n}"), 42);
    run_case("var", MAIN("{\n  var n = 1\n  n = n + 1\n  n = n * 10\n  n\n}"), 20);
    run_case("while sum", MAIN("{\n  var i = 0\n  var t = 0\n  while i <= 100 { t = t + i\n i = i + 1 }\n  t\n}"), 5050);
    run_case("factorial", MAIN("{\n  var i = 1\n  var f = 1\n  while i <= 5 { f = f * i\n i = i + 1 }\n  f\n}"), 120);
    run_case("nested loops", MAIN("{\n  var c = 0\n  var i = 0\n  while i < 5 {\n    var j = 0\n    while j < 5 { c = c + 1\n j = j + 1 }\n    i = i + 1\n  }\n  c\n}"), 25);
    run_case("block value", MAIN("{ let a = { 3 }\n a + { 4 } }"), 7);
    run_case("scope reuse", MAIN("{\n  { let a = 1 }\n  let a = 5\n  a\n}"), 5);
    run_case("void main", "func main() = {}\n", 0);
    run_case("many locals", MAIN("{\n  let a = 1\n  let b = 2\n  let c = 3\n  let d = 4\n  let e = 5\n  let f = 6\n  a + b * 2 + c * 3 + d * 4 + e * 5 + f * 6\n}"), 91);
}

static void test_functions(void) {
    run_case("call", "func add(a: i32, b: i32) -> i32 = a + b\n" MAIN("add(40, 2)"), 42);
    run_case("arg order", "func sub(a: i32, b: i32) -> i32 = a - b\n" MAIN("sub(10, 3)"), 7);
    run_case("fib", "func fib(n: i32) -> i32 = {\n  if n < 2 { return n }\n  fib(n - 1) + fib(n - 2)\n}\n" MAIN("fib(10)"), 55);
    run_case("fact rec", "func f(n: i32) -> i32 = if n <= 1 { 1 } else { n * f(n - 1) }\n" MAIN("f(5)"), 120);
    run_case("nested calls", "func add(a: i32, b: i32) -> i32 = a + b\n" MAIN("add(add(1, 2), add(3, add(4, 5)))"), 15);
    run_case("six args", "func f(a: i32, b: i32, c: i32, d: i32, e: i32, g: i32) -> i32 = a + 2 * b + 3 * c + 4 * d + 5 * e + 6 * g\n" MAIN("f(1, 2, 3, 4, 5, 6)"), 91);
    run_case("mutual", "func even(n: i32) -> bool = if n == 0 { true } else { odd(n - 1) }\nfunc odd(n: i32) -> bool = if n == 0 { false } else { even(n - 1) }\n" MAIN("if even(10) { 1 } else { 0 }"), 1);
    run_case("early return", "func abs(x: i32) -> i32 = {\n  if x < 0 { return 0 - x }\n  x\n}\n" MAIN("abs(-5) + abs(3)"), 8);
    run_case("return in loop", "func first(n: i32) -> i32 = {\n  var i = 0\n  while i < 100 {\n    if i * i > n { return i }\n    i = i + 1\n  }\n  0\n}\n" MAIN("first(50)"), 8);
    run_case("small params", "func f(a: u8, b: i8) -> i32 = (a as i32) + (b as i32)\n" MAIN("f(200, -3)"), 197);
    run_case("bool param", "func f(b: bool, x: i32) -> i32 = if b { x } else { 0 - x }\n" MAIN("f(false, 9) + f(true, 20)"), 11);
    run_case("param assign local copy", "func f(x: i32) -> i32 = {\n  var y = x\n  y = y + 1\n  y\n}\n" MAIN("f(4)"), 5);
    run_case("void call", "func nothing() = {}\n" MAIN("{\n  nothing()\n  9\n}"), 9);
    run_case("namespaces", "struct lib = {\n  export struct m = {\n    export func twice(a: i32) -> i32 = a + a\n  }\n}\n" MAIN("lib.m.twice(21)"), 42);
    run_case("inner call", "struct m = {\n  func h(a: i32) -> i32 = a + 1\n  export func g(a: i32) -> i32 = h(a) * 2\n}\n" MAIN("m.g(4)"), 10);
}

#define P_STRUCT "struct P = { x: i32, y: i32 }\n"

static void test_pointers(void) {
    run_case("store through pointer", MAIN("{\n  var x = 5\n  let p = &x\n  *p = 9\n  x\n}"), 9);
    run_case("load through pointer", MAIN("{\n  let x = 7\n  let p = &x\n  *p + 1\n}"), 8);
    run_case("pointer param", "func inc(p: *i32) = { *p = *p + 1 }\n" MAIN("{\n  var n = 1\n  inc(&n)\n  inc(&n)\n  n\n}"), 3);
    run_case("swap",
             "func swap(a: *i32, b: *i32) = {\n  let t = *a\n  *a = *b\n  *b = t\n}\n"
             MAIN("{\n  var x = 1\n  var y = 2\n  swap(&x, &y)\n  x * 10 + y\n}"), 21);
    run_case("pointer to pointer", MAIN("{\n  var x = 3\n  var p = &x\n  let pp = &p\n  **pp = 8\n  x\n}"), 8);
    run_case("i8 through pointer", MAIN("{\n  var b: i8 = -3\n  let p = &b\n  *p as i32 + 10\n}"), 7);
    run_case("u8 write", MAIN("{\n  var b: u8 = 255\n  let p = &b\n  *p = 1\n  b as i32\n}"), 1);
    run_case("i8 write", MAIN("{\n  var b: i8 = -1\n  let p = &b\n  *p = -2\n  b as i32\n}"), -2);
    run_case("pointer compare",
             MAIN("{\n  var a = 1\n  var b = 2\n  let p = &a\n  let q = &a\n  let r = &b\n  if p == q && p != r { 1 } else { 0 }\n}"), 1);
    run_case("pointer cast", MAIN("{\n  var a = 5\n  let p = &a\n  let u = p as usize\n  let q = u as *i32\n  *q\n}"), 5);
}

static void test_structs(void) {
    run_case("fields", P_STRUCT MAIN("{\n  let p = P { x: 3, y: 4 }\n  p.x * p.x + p.y * p.y\n}"), 25);
    run_case("field assign", P_STRUCT MAIN("{\n  var p = P { x: 1, y: 2 }\n  p.x = 10\n  p.y = p.y + 5\n  p.x + p.y\n}"), 17);
    run_case("through pointer", P_STRUCT "func sum(p: *P) -> i32 = p.x + p.y\n"
             MAIN("{\n  let p = P { x: 3, y: 4 }\n  sum(&p)\n}"), 7);
    run_case("mutate through pointer", P_STRUCT "func zero(p: *P) = {\n  p.x = 0\n  p.y = 0\n}\n"
             MAIN("{\n  var p = P { x: 3, y: 4 }\n  zero(&p)\n  p.x + p.y + 1\n}"), 1);
    run_case("pass by value", P_STRUCT "func f(p: P) -> i32 = p.x - p.y\n" MAIN("f(P { x: 10, y: 3 })"), 7);
    run_case("big struct then arg",
             "struct Big = { a: i32, b: i32, c: i32, d: i32, e: i32 }\n"
             "func g(x: Big, k: i32) -> i32 = x.a + x.e * 2 + k\n"
             MAIN("g(Big { a: 1, b: 0, c: 0, d: 0, e: 5 }, 100)"), 111);
    run_case("arg then big struct",
             "struct Big = { a: i32, b: i32, c: i32, d: i32, e: i32 }\n"
             "func h(k: i32, x: Big) -> i32 = k * 100 + x.e\n"
             MAIN("h(2, Big { a: 1, b: 0, c: 0, d: 0, e: 7 })"), 207);
    run_case("return struct", P_STRUCT "func mk(a: i32, b: i32) -> P = P { x: a, y: b }\n"
             MAIN("{\n  let p = mk(3, 5)\n  p.x * 10 + p.y\n}"), 35);
    run_case("return local struct",
             P_STRUCT "func mk2() -> P = {\n  var p = P { x: 1, y: 2 }\n  p.x = 7\n  p\n}\n"
             MAIN("{\n  let p = mk2()\n  p.x * 10 + p.y\n}"), 72);
    run_case("field of call result", P_STRUCT "func mk(a: i32, b: i32) -> P = P { x: a, y: b }\n" MAIN("mk(3, 5).y"), 5);
    run_case("nested struct",
             P_STRUCT "struct R = { o: P, w: i32, h: i32 }\n"
             MAIN("{\n  var r = R { o: P { x: 1, y: 2 }, w: 3, h: 4 }\n  r.o.y = 9\n  r.o.x + r.o.y + r.w * r.h\n}"), 22);
    run_case("pass nested field by value",
             P_STRUCT "struct R = { o: P, w: i32 }\nfunc gx(p: P) -> i32 = p.x * 10 + p.y\n"
             MAIN("{\n  let r = R { o: P { x: 4, y: 2 }, w: 1 }\n  gx(r.o)\n}"), 42);
    run_case("mixed layout",
             "struct M = { a: u8, b: i32, c: u8, d: i64 }\n"
             MAIN("{\n  let m = M { a: 1, b: 2, c: 3, d: 4 }\n  (m.a as i32) + m.b + (m.c as i32) + (m.d as i32)\n}"), 10);
    run_case("sign extension of fields",
             "struct S = { a: i8, b: u8 }\n"
             MAIN("{\n  let s = S { a: -1, b: 255 }\n  (s.a as i32) + (s.b as i32)\n}"), 254);
    run_case("struct copy", P_STRUCT MAIN("{\n  var a = P { x: 1, y: 2 }\n  var b = a\n  b.x = 9\n  a.x * 10 + b.x\n}"), 19);
    run_case("struct assign", P_STRUCT MAIN("{\n  var a = P { x: 1, y: 2 }\n  var b = P { x: 5, y: 6 }\n  a = b\n  a.x + a.y\n}"), 11);
    run_case("pointer to field", P_STRUCT MAIN("{\n  var p = P { x: 1, y: 2 }\n  let q = &p.y\n  *q = 8\n  p.x + p.y\n}"), 9);
}

static void test_pointer_indexing(void) {
    run_case("index i32", P_STRUCT MAIN("{\n  var s = P { x: 3, y: 4 }\n  let p = &s.x\n  p[0] * 10 + p[1]\n}"), 34);
    run_case("index i64", "struct Q = { a: i64, b: i64 }\n"
             MAIN("{\n  var q = Q { a: 3, b: 4 }\n  let p = &q.a\n  (p[0] * 10 + p[1]) as i32\n}"), 34);
    run_case("index u8", "struct B = { a: u8, b: u8, c: u8 }\n"
             MAIN("{\n  var b = B { a: 1, b: 2, c: 3 }\n  let p = &b.a\n  (p[0] as i32) + (p[1] as i32) * 10 + (p[2] as i32) * 100\n}"), 321);
    run_case("index store", P_STRUCT MAIN("{\n  var s = P { x: 3, y: 4 }\n  let p = &s.x\n  p[1] = 7\n  s.x + s.y\n}"), 10);
    run_case("index variable", P_STRUCT MAIN("{\n  var s = P { x: 3, y: 4 }\n  let p = &s.x\n  var i = 1\n  p[i] + p[i - 1]\n}"), 7);
}

static void test_methods(void) {
    const char *counter =
        "struct Counter = {\n"
        "  n: i32\n"
        "  export func inc(c: *Counter) = { c.n = c.n + 1 }\n"
        "  export func get(c: Counter) -> i32 = c.n\n"
        "  export func add(c: *Counter, k: i32) -> i32 = c.n + k\n"
        "  export func make(k: i32) -> Counter = Counter { n: k }\n"
        "}\n";
    char src[1200];

    snprintf(src, sizeof src, "%s" MAIN("{\n  var c = Counter { n: 0 }\n  c.inc()\n  c.inc()\n  c.add(5) + c.get()\n}"), counter);
    run_case("methods", src, 9);
    snprintf(src, sizeof src, "%s" "func bump(c: *Counter) = { c.inc()\n c.inc() }\n"
             MAIN("{\n  var c = Counter { n: 10 }\n  bump(&c)\n  c.get()\n}"), counter);
    run_case("method through a pointer", src, 12);
    snprintf(src, sizeof src, "%s" "func read(c: *Counter) -> i32 = c.get()\n"
             MAIN("{\n  let c = Counter { n: 6 }\n  read(&c)\n}"), counter);
    run_case("receiver is dereferenced", src, 6);
    snprintf(src, sizeof src, "%s" MAIN("{\n  let c = Counter.make(5)\n  c.get() + Counter.make(2).get()\n}"), counter);
    run_case("function in a struct", src, 7);
}

static void test_defer_and_short_circuit(void) {
    run_case("defer runs at the end",
             "func f(p: *i32) = {\n  defer *p = *p * 2\n  *p = *p + 3\n}\n"
             MAIN("{\n  var n = 1\n  f(&n)\n  n\n}"), 8);
    run_case("defer order is last in, first out",
             "func f(p: *i32) = {\n  defer *p = *p * 10\n  defer *p = *p + 1\n  *p = 5\n}\n"
             MAIN("{\n  var n = 0\n  f(&n)\n  n\n}"), 60);
    run_case("defer with return",
             "func g(p: *i32) -> i32 = {\n  defer *p = 99\n  return 7\n}\n"
             MAIN("{\n  var n = 0\n  let r = g(&n)\n  r + n\n}"), 106);
    run_case("defer in a nested block",
             MAIN("{\n  var n = 0\n  {\n    defer n = n + 1\n    n = n + 10\n  }\n  n\n}"), 11);
    run_case("return in a loop runs defers",
             "func h(p: *i32) -> i32 = {\n  defer *p = *p + 1\n  var i = 0\n  while i < 10 {\n    if i == 3 { return i }\n    i = i + 1\n  }\n  0\n}\n"
             MAIN("{\n  var n = 0\n  let r = h(&n)\n  r * 10 + n\n}"), 31);
    run_case("defer keeps the block value",
             "func f(p: *i32) -> i32 = {\n  defer *p = 50\n  *p + 1\n}\n"
             MAIN("{\n  var n = 4\n  let r = f(&n)\n  r * 10 + n / 10\n}"), 55);
    run_case("and short-circuits",
             "func bump(p: *i32) -> bool = {\n  *p = *p + 1\n  true\n}\n"
             MAIN("{\n  var n = 0\n  if false && bump(&n) { }\n  if true || bump(&n) { }\n  n\n}"), 0);
    run_case("and evaluates the right side",
             "func bump(p: *i32) -> bool = {\n  *p = *p + 1\n  true\n}\n"
             MAIN("{\n  var n = 0\n  if true && bump(&n) { }\n  if false || bump(&n) { }\n  n\n}"), 2);
    compile_error("return in defer",
                  "func f() -> i32 = {\n  defer { return 1 }\n  2\n}\n" MAIN("f()"),
                  "'return' is not allowed inside a deferred expression");
}

/* The examples in examples/checked/ compile and give these exit statuses. */
static void test_examples(void) {
    static const struct {
        const char *path;
        int want;
    } files[] = {
        {"examples/checked/fib.gale", 55},
        {"examples/checked/sum.gale", 5050 & 255},
        {"examples/checked/geometry.gale", 12},
    };
    size_t i;

    for (i = 0; i < sizeof files / sizeof files[0]; i++) {
        char *src = read_file(files[i].path);
        if (!src) {
            fprintf(stderr, "cannot read %s (run the tests from the repository root)\n", files[i].path);
            failures++;
            continue;
        }
        run_case(files[i].path, src, files[i].want);
        free(src);
    }
}

static void test_reachability(void) {
    Build b;

    /* functions that are never called are not generated, so they may use things the
     * generator cannot do yet */
    run_case("unused function", "func unused() -> f64 = 1.5\n" MAIN("3"), 3);

    if (build(MAIN("1") "func other() -> i32 = 2\n", &b)) {
        if (strstr(b.asm_text, "other")) {
            fprintf(stderr, "an unused function was generated:\n%s", b.asm_text);
            failures++;
        }
    } else {
        fprintf(stderr, "reachability: %s\n", b.msg);
        failures++;
    }
    release(&b);
}

static void test_output(void) {
    Build b;

    if (!build(MAIN("42"), &b)) {
        fprintf(stderr, "output: %s\n", b.msg);
        failures++;
        release(&b);
        return;
    }
    if (strcmp(b.asm_text,
               "_start:\n    call main\n    mov rdi, rax\n    mov rax, 60\n    syscall\n"
               "main:\n    push rbp\n    mov rbp, rsp\n    mov rax, 42\n.L2:\n    mov rsp, rbp\n"
               "    pop rbp\n    ret\n") != 0) {
        fprintf(stderr, "unexpected assembly:\n%s", b.asm_text);
        failures++;
    }
    /* headers + _start (call 5 + mov 3 + mov eax,60 5 + syscall 2 = 15 bytes)
     * + main (push 1 + mov 3 + mov eax,42 5 + mov 3 + pop 1 + ret 1 = 14 bytes) */
    if (b.elf_len != ELF_HEADERS_SIZE + 15 + 14) {
        fprintf(stderr, "unexpected ELF size %lu\n", (unsigned long)b.elf_len);
        failures++;
    }
    release(&b);
}

static void test_errors(void) {
    compile_error("no main", "func f() -> i32 = 1\n", "there is no 'main' function");
    compile_error("main with params", "func main(a: i32) -> i32 = a\n", "'main' cannot take parameters");
    compile_error("main returns struct", "struct P = { x: i32 }\nfunc main() -> P = P { x: 1 }\n",
                  "'main' must return void or an integer");
    compile_error("float", MAIN("{\n  let x: f64 = 1.5\n  0\n}"), "does not support f64 yet");
    compile_error("sema error is reported", "func main() -> i32 = true\n", "the body has type bool");
}

int main(void) {
    test_arithmetic();
    test_integer_widths();
    test_comparisons_and_logic();
    test_variables_and_loops();
    test_functions();
    test_pointers();
    test_structs();
    test_pointer_indexing();
    test_methods();
    test_defer_and_short_circuit();
    test_examples();
    test_reachability();
    test_output();
    test_errors();

    if (failures) {
        fprintf(stderr, "%d test(s) failed\n", failures);
        return 1;
    }
    printf("codegen: all tests passed (%d programs run)\n", ran);
    return 0;
}
