#ifndef GALE_TY_H
#define GALE_TY_H

#include <stddef.h>

#include "arena.h"

/* Semantic types. Types are interned in a TyCtx, so two types are the same type
 * exactly when their pointers are equal. (`isize`/`usize` are distinct from
 * `i64`/`u64`; all of them are 64 bits wide for now.) */

typedef struct StructInfo StructInfo; /* defined in sema.h */
typedef struct Ty Ty;

typedef enum {
    TY_VOID,
    TY_NEVER, /* type of code that does not finish normally (a `return`) */
    TY_BOOL,
    TY_INT,
    TY_FLOAT,
    TY_PTR,
    TY_SLICE,
    TY_ARRAY,
    TY_OPTIONAL,
    TY_ERRUNION,
    TY_STRUCT,
    TY_FUNC
} TyKind;

struct Ty {
    TyKind kind;
    const char *name; /* scalars and structs */
    int bits;         /* INT, FLOAT */
    int is_signed;    /* INT */
    Ty *elem;         /* PTR, SLICE, ARRAY, OPTIONAL, ERRUNION */
    size_t len;       /* ARRAY */
    StructInfo *strukt; /* STRUCT */
    Ty **params;      /* FUNC */
    size_t nparams;
    Ty *ret;
};

typedef struct {
    Arena *arena;
    Ty *void_, *never, *bool_;
    Ty *i8, *i16, *i32, *i64, *isize;
    Ty *u8, *u16, *u32, *u64, *usize;
    Ty *f32, *f64;
    Ty *str; /* []u8, the type of string literals */
    Vec composite; /* interned Ty* */
} TyCtx;

void ty_ctx_init(TyCtx *c, Arena *arena);

/* The builtin scalar type with this name (i32, bool, void, ...), or NULL. */
Ty *ty_builtin(TyCtx *c, const char *name, size_t len);

Ty *ty_ptr(TyCtx *c, Ty *elem);
Ty *ty_slice(TyCtx *c, Ty *elem);
Ty *ty_array(TyCtx *c, Ty *elem, size_t len);
Ty *ty_optional(TyCtx *c, Ty *elem);
Ty *ty_errunion(TyCtx *c, Ty *elem);
Ty *ty_func(TyCtx *c, Ty **params, size_t nparams, Ty *ret);
/* A new, distinct struct type (one per declaration). */
Ty *ty_new_struct(TyCtx *c, const char *name, size_t name_len, StructInfo *info);

int ty_is_integer(const Ty *t);
int ty_is_float(const Ty *t);
int ty_is_numeric(const Ty *t);

/* Can a value of type `from` be used where `to` is expected? Same type, a value of
 * type `never`, or T where ?T / T! is expected. */
int ty_assignable(const Ty *from, const Ty *to);

/* Readable name, e.g. "*[]u8" (allocated in the arena). */
const char *ty_str(TyCtx *c, const Ty *t);

#endif /* GALE_TY_H */
