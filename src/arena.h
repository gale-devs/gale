#ifndef GALE_ARENA_H
#define GALE_ARENA_H

#include <stddef.h>

/* Bump allocator. Everything is freed at once with arena_free(). */

typedef struct ArenaChunk ArenaChunk;

typedef struct {
    ArenaChunk *head;
} Arena;

void arena_init(Arena *a);

/* Returns zeroed, 8-byte aligned memory. Exits the process on out-of-memory. */
void *arena_alloc(Arena *a, size_t size);

void arena_free(Arena *a);

/* Growable array whose storage lives in an arena (old buffers are not reclaimed
 * until the arena is freed). */
typedef struct {
    void *data;
    size_t len;
    size_t cap;
    size_t elem_size;
} Vec;

void vec_init(Vec *v, size_t elem_size);
void vec_push(Arena *a, Vec *v, const void *elem);

#endif /* GALE_ARENA_H */
