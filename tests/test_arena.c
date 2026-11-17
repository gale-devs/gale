#include <stdint.h>
#include <stdio.h>

#include "arena.h"

static int failures = 0;

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            failures++;                                                          \
        }                                                                        \
    } while (0)

static void test_alloc(void) {
    Arena a;
    unsigned char *p, *q, *big;
    size_t i;

    arena_init(&a);
    p = arena_alloc(&a, 3);
    q = arena_alloc(&a, 5);
    CHECK(p != NULL && q != NULL && p != q);
    CHECK(((uintptr_t)p & 7u) == 0);
    CHECK(((uintptr_t)q & 7u) == 0);
    for (i = 0; i < 3; i++) {
        CHECK(p[i] == 0);
    }

    /* larger than a chunk */
    big = arena_alloc(&a, 100000);
    CHECK(big != NULL);
    for (i = 0; i < 100000; i += 997) {
        CHECK(big[i] == 0);
    }
    big[99999] = 7;
    CHECK(arena_alloc(&a, 0) != NULL);
    arena_free(&a);
}

static void test_vec_ints(void) {
    Arena a;
    Vec v;
    int i;

    arena_init(&a);
    vec_init(&v, sizeof(int));
    for (i = 0; i < 1000; i++) {
        vec_push(&a, &v, &i);
    }
    CHECK(v.len == 1000);
    for (i = 0; i < 1000; i++) {
        CHECK(((int *)v.data)[i] == i);
    }
    arena_free(&a);
}

static void test_vec_pointers(void) {
    Arena a;
    Vec v;
    int xs[10];
    int i;

    arena_init(&a);
    vec_init(&v, sizeof(int *));
    for (i = 0; i < 10; i++) {
        int *p = &xs[i];
        xs[i] = i * i;
        vec_push(&a, &v, &p);
    }
    CHECK(v.len == 10);
    for (i = 0; i < 10; i++) {
        CHECK(*((int **)v.data)[i] == i * i);
    }
    arena_free(&a);
}

int main(void) {
    test_alloc();
    test_vec_ints();
    test_vec_pointers();

    if (failures) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    printf("arena: all tests passed\n");
    return 0;
}
