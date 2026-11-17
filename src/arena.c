#include "arena.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct ArenaChunk {
    ArenaChunk *next;
    size_t used;
    size_t cap;
};

#define CHUNK_MIN ((size_t)4096)

static size_t align8(size_t n) {
    return (n + 7u) & ~(size_t)7u;
}

static void oom(void) {
    fprintf(stderr, "gale: out of memory\n");
    exit(1);
}

void arena_init(Arena *a) {
    a->head = NULL;
}

void *arena_alloc(Arena *a, size_t size) {
    size_t need = align8(size ? size : 1);
    ArenaChunk *c = a->head;
    char *p;

    if (!c || c->cap - c->used < need) {
        size_t cap = need > CHUNK_MIN ? need : CHUNK_MIN;
        c = malloc(align8(sizeof(ArenaChunk)) + cap);
        if (!c) {
            oom();
        }
        c->next = a->head;
        c->used = 0;
        c->cap = cap;
        a->head = c;
    }
    p = (char *)c + align8(sizeof(ArenaChunk)) + c->used;
    c->used += need;
    memset(p, 0, need);
    return p;
}

void arena_free(Arena *a) {
    ArenaChunk *c = a->head;
    while (c) {
        ArenaChunk *next = c->next;
        free(c);
        c = next;
    }
    a->head = NULL;
}

void vec_init(Vec *v, size_t elem_size) {
    v->data = NULL;
    v->len = 0;
    v->cap = 0;
    v->elem_size = elem_size;
}

void vec_push(Arena *a, Vec *v, const void *elem) {
    if (v->len == v->cap) {
        size_t ncap = v->cap ? v->cap * 2 : 4;
        void *nd = arena_alloc(a, ncap * v->elem_size);
        if (v->len) {
            memcpy(nd, v->data, v->len * v->elem_size);
        }
        v->data = nd;
        v->cap = ncap;
    }
    memcpy((char *)v->data + v->len * v->elem_size, elem, v->elem_size);
    v->len++;
}
