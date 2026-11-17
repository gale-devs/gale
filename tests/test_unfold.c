#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "arena.h"
#include "ast.h"
#include "fsutil.h"
#include "parser.h"
#include "unfold.h"

/* Run from the repository root (make test does). */
#define FIX "tests/fixtures/unfold"
#define ALT "tests/fixtures/unfold_alt"

#define MATH_BODY " (export func add ((a i32) (b i32)) i32 (+ a b)) (func hidden () void 0))"
#define MATH_DUMP "(struct math" MATH_BODY
#define MATH_EXPORTED "(export struct math" MATH_BODY

static int failures = 0;

static char *copy(const char *s) {
    char *c = malloc(strlen(s) + 1);
    if (!c) {
        exit(1);
    }
    strcpy(c, s);
    return c;
}

/* Parses and expands `entry`. Returns a malloc'ed string: the dump of the expanded
 * tree, or "ERROR file:line:col: message". */
static char *expand(const char *entry, const char *const *roots, size_t nroots) {
    Arena a;
    ParseError pe;
    UnfoldError ue;
    File *f;
    Unfolder *u;
    char *src = read_file(entry);
    char *out;
    char buf[1200];

    if (!src) {
        snprintf(buf, sizeof buf, "ERROR cannot read %s", entry);
        return copy(buf);
    }
    arena_init(&a);
    f = parse_file(src, &a, &pe);
    if (!f) {
        snprintf(buf, sizeof buf, "ERROR %s:%d:%d: %s", entry, pe.line, pe.col, pe.msg);
        out = copy(buf);
    } else {
        u = unfold_new(&a, roots, nroots);
        if (unfold_expand(u, f, entry, &ue)) {
            out = ast_dump(f);
        } else {
            snprintf(buf, sizeof buf, "ERROR %s:%d:%d: %s", ue.file, ue.line, ue.col, ue.msg);
            out = copy(buf);
        }
        unfold_free(u);
    }
    arena_free(&a);
    free(src);
    return out;
}

static void check(const char *name, const char *want) {
    char path[200];
    const char *roots[1];
    char *got;

    snprintf(path, sizeof path, FIX "/ex/%s.gale", name);
    roots[0] = FIX;
    got = expand(path, roots, 1);
    if (strcmp(got, want) != 0) {
        fprintf(stderr, "%s: mismatch\n  got:  %s\n  want: %s\n", name, got, want);
        failures++;
    }
    free(got);
}

static void check_error(const char *name, const char *needle) {
    char path[200];
    const char *roots[1];
    char *got;

    snprintf(path, sizeof path, FIX "/ex/%s.gale", name);
    roots[0] = FIX;
    got = expand(path, roots, 1);
    if (strncmp(got, "ERROR ", 6) != 0 || !strstr(got, needle)) {
        fprintf(stderr, "%s: expected an error containing \"%s\", got:\n  %s\n", name, needle, got);
        failures++;
    }
    free(got);
}

static void test_basic(void) {
    check("basic",
          "(func main () void (call (. (. lib math) add) 1 2))\n"
          "(struct lib " MATH_DUMP ")");
    check("inside", "(struct S (x i32) (struct lib " MATH_DUMP "))");
}

static void test_merge(void) {
    /* two unfolds under the same path land in one struct */
    check("merge",
          "(struct lib " MATH_DUMP " (struct str (export func len ((s (slice u8))) usize 0)))");
    /* merged into a struct the file already defines */
    check("userstruct", "(struct lib (n i32) " MATH_DUMP ")");
}

static void test_repeat_and_export(void) {
    check("twice", "(struct lib " MATH_DUMP ")");
    check("export", "(export struct lib " MATH_EXPORTED ")");
    /* a repeated unfold is ignored, but `export unfold` still exports it */
    check("twice_export", "(export struct lib " MATH_EXPORTED ")");
}

static void test_nested_files(void) {
    /* lib/net.gale unfolds lib.math itself: the result sits inside net */
    check("nested",
          "(struct lib (struct net (export func ping () void (call (. (. lib math) add) 1 1)) "
          "(struct lib " MATH_DUMP ")))");
}

static void test_partial(void) {
    check("partial", "(struct lib (struct math (export func add ((a i32) (b i32)) i32 (+ a b))))");
    check("partial_deep", "(struct lib (struct deep (struct Inner (func f () void 1))))");
}

static void test_roots(void) {
    const char *roots[2];
    char *got;

    roots[0] = FIX;
    roots[1] = ALT;
    got = expand(FIX "/ex/roots.gale", roots, 2);
    if (strcmp(got, "(struct lib " MATH_DUMP ")\n"
                    "(struct alt (struct util (export func helper () void 1)))") != 0) {
        fprintf(stderr, "roots: mismatch\n  got: %s\n", got);
        failures++;
    }
    free(got);

    /* the second root is not searched when it is not given */
    got = expand(FIX "/ex/roots.gale", roots, 1);
    if (strncmp(got, "ERROR ", 6) != 0 || !strstr(got, "cannot find 'alt.util'")) {
        fprintf(stderr, "roots: expected a not-found error, got: %s\n", got);
        failures++;
    }
    free(got);

    got = expand(FIX "/ex/basic.gale", roots, 0);
    if (!strstr(got, "no search directories")) {
        fprintf(stderr, "roots: expected 'no search directories', got: %s\n", got);
        failures++;
    }
    free(got);
}

static void test_errors(void) {
    /* the error points at the unfold in the file that wrote it */
    check_error("conflict", "ex/conflict.gale:1:1:");
    check_error("conflict", "func 'lib.math.add' is defined more than once");
    check_error("kindclash", "'lib' is defined as both a func and a struct");
    check_error("fieldclash", "field 'lib.rec.Rec.x' is already defined");
    check_error("missing", "cannot find 'lib.nothing'");
    check_error("missing", "lib/nothing.gale");
    check_error("dir", "cannot find 'lib'");
    check_error("missing_member", "cannot find 'nothing' in 'lib.math'");
    check_error("missing_struct", "cannot find 'Nope' in 'lib.deep'");
    /* a syntax error is reported against the file that contains it */
    check_error("badfile", "lib/bad.gale:1:6: expected a function name");
}

static void test_cycles(void) {
    check_error("cycle", "circular unfold");
    check_error("cycle", "a.gale -> ");
    check_error("self", "circular unfold");
}

int main(void) {
    test_basic();
    test_merge();
    test_repeat_and_export();
    test_nested_files();
    test_partial();
    test_roots();
    test_errors();
    test_cycles();

    if (failures) {
        fprintf(stderr, "%d test(s) failed\n", failures);
        return 1;
    }
    printf("unfold: all tests passed\n");
    return 0;
}
