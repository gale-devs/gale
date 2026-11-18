#include <stdio.h>
#include <string.h>

#include "arena.h"
#include "ty.h"

static int failures = 0;

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            failures++;                                                          \
        }                                                                        \
    } while (0)

static void test_interning(void) {
    Arena a;
    TyCtx c;
    Ty *params[2];

    arena_init(&a);
    ty_ctx_init(&c, &a);

    CHECK(ty_ptr(&c, c.i32) == ty_ptr(&c, c.i32));
    CHECK(ty_ptr(&c, c.i32) != ty_ptr(&c, c.u32));
    CHECK(ty_slice(&c, c.u8) == c.str);
    CHECK(ty_array(&c, c.i32, 4) == ty_array(&c, c.i32, 4));
    CHECK(ty_array(&c, c.i32, 4) != ty_array(&c, c.i32, 5));
    CHECK(ty_optional(&c, c.i32) != ty_errunion(&c, c.i32));
    CHECK(ty_ptr(&c, ty_ptr(&c, c.i8)) == ty_ptr(&c, ty_ptr(&c, c.i8)));

    params[0] = c.i32;
    params[1] = ty_ptr(&c, c.u8);
    CHECK(ty_func(&c, params, 2, c.bool_) == ty_func(&c, params, 2, c.bool_));
    CHECK(ty_func(&c, params, 2, c.bool_) != ty_func(&c, params, 2, c.void_));
    CHECK(ty_func(&c, params, 1, c.bool_) != ty_func(&c, params, 2, c.bool_));
    CHECK(ty_func(&c, NULL, 0, c.void_) == ty_func(&c, NULL, 0, c.void_));

    /* i64 and isize are different types */
    CHECK(c.i64 != c.isize);
    CHECK(c.u64 != c.usize);
    arena_free(&a);
}

static void test_builtins(void) {
    Arena a;
    TyCtx c;

    arena_init(&a);
    ty_ctx_init(&c, &a);
    CHECK(ty_builtin(&c, "i32", 3) == c.i32);
    CHECK(ty_builtin(&c, "usize", 5) == c.usize);
    CHECK(ty_builtin(&c, "f64", 3) == c.f64);
    CHECK(ty_builtin(&c, "bool", 4) == c.bool_);
    CHECK(ty_builtin(&c, "void", 4) == c.void_);
    CHECK(ty_builtin(&c, "i3", 2) == NULL);
    CHECK(ty_builtin(&c, "i32x", 4) == NULL);
    CHECK(ty_builtin(&c, "never", 5) == NULL); /* not nameable in source */
    CHECK(ty_is_integer(c.u8) && ty_is_integer(c.isize) && !ty_is_integer(c.f32));
    CHECK(ty_is_float(c.f64) && !ty_is_float(c.i32));
    CHECK(ty_is_numeric(c.i8) && ty_is_numeric(c.f32) && !ty_is_numeric(c.bool_));
    arena_free(&a);
}

static void test_assignable(void) {
    Arena a;
    TyCtx c;

    arena_init(&a);
    ty_ctx_init(&c, &a);
    CHECK(ty_assignable(c.i32, c.i32));
    CHECK(!ty_assignable(c.i32, c.i64));
    CHECK(!ty_assignable(c.i32, c.u32));
    CHECK(ty_assignable(c.never, c.i32));
    CHECK(ty_assignable(c.never, c.void_));
    CHECK(ty_assignable(c.i32, ty_optional(&c, c.i32)));
    CHECK(ty_assignable(c.i32, ty_errunion(&c, c.i32)));
    CHECK(!ty_assignable(c.i64, ty_optional(&c, c.i32)));
    CHECK(!ty_assignable(ty_optional(&c, c.i32), c.i32));
    CHECK(ty_assignable(ty_ptr(&c, c.u8), ty_ptr(&c, c.u8)));
    CHECK(!ty_assignable(ty_ptr(&c, c.u8), ty_ptr(&c, c.i8)));
    arena_free(&a);
}

static void test_names(void) {
    Arena a;
    TyCtx c;
    Ty *params[2];

    arena_init(&a);
    ty_ctx_init(&c, &a);
    CHECK(strcmp(ty_str(&c, c.i32), "i32") == 0);
    CHECK(strcmp(ty_str(&c, c.str), "[]u8") == 0);
    CHECK(strcmp(ty_str(&c, ty_ptr(&c, ty_slice(&c, c.u8))), "*[]u8") == 0);
    CHECK(strcmp(ty_str(&c, ty_array(&c, ty_optional(&c, c.i32), 3)), "[3]?i32") == 0);
    CHECK(strcmp(ty_str(&c, ty_errunion(&c, c.usize)), "usize!") == 0);
    params[0] = c.i32;
    params[1] = ty_ptr(&c, c.f64);
    CHECK(strcmp(ty_str(&c, ty_func(&c, params, 2, c.bool_)), "func(i32, *f64) -> bool") == 0);
    CHECK(strcmp(ty_str(&c, ty_new_struct(&c, "Point", 5, NULL)), "Point") == 0);
    arena_free(&a);
}

int main(void) {
    test_interning();
    test_builtins();
    test_assignable();
    test_names();

    if (failures) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    printf("ty: all tests passed\n");
    return 0;
}
