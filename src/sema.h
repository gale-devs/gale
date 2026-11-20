#ifndef GALE_SEMA_H
#define GALE_SEMA_H

#include "arena.h"
#include "ast.h"
#include "ty.h"

/* Name resolution and type checking. Works on a tree whose `unfold`s have already
 * been expanded. On success the tree is annotated: every Expr has `ty` (and `sym`
 * when it names something), every Item has `sym`, every let/var has `sym`. */

typedef enum {
    SYM_STRUCT,
    SYM_FUNC,
    SYM_LOCAL
} SymKind;

typedef struct {
    Str name;
    Ty *ty;
    int line, col;
} FieldInfo;

struct StructInfo {
    Str name;           /* empty for the file itself */
    Item *item;         /* NULL for the file itself */
    StructInfo *parent; /* NULL for the file itself */
    Ty *ty;             /* NULL for the file itself */
    FieldInfo *fields;
    size_t nfields;
    Sym **members;      /* the structs and funcs declared in the body, in source order */
    size_t nmembers;
    int state;          /* 0 = not visited, 1 = visiting, 2 = done (recursion check) */

    /* layout, filled in lazily by ty_layout() */
    int laid_out;
    size_t size, align;
    size_t *offsets;    /* offset of each field */
};

struct Sym {
    SymKind kind;
    Str name;
    Item *item;         /* STRUCT, FUNC */
    StructInfo *owner;  /* STRUCT, FUNC: the struct whose body declares it */
    StructInfo *strukt; /* STRUCT: the struct it declares */
    Ty *ty;             /* STRUCT: its type, FUNC: its function type, LOCAL: its type */
    int is_mutable;     /* LOCAL: declared with `var` */
    int is_param;       /* LOCAL */

    /* used by the code generator */
    long frame_off;     /* LOCAL: offset from rbp */
    int label;          /* FUNC: its assembly label (valid if has_label) */
    int has_label;
};

typedef struct {
    char file[512]; /* file the error is in ("" if unknown, e.g. the tree was not tagged) */
    char msg[300];
    int line, col;
} SemaError;

typedef struct Sema Sema;

Sema *sema_new(Arena *arena);

/* Checks the whole file. Returns 1 on success; on failure returns 0 and fills *err
 * (the first error only). */
int sema_check(Sema *s, File *file, SemaError *err);

/* Value of an integer literal's text (decimal, 0x, 0b, with '_'). Returns 0 if it
 * does not fit in 64 bits. */
int parse_int_literal(Str text, unsigned long long *out);

TyCtx *sema_types(Sema *s);
StructInfo *sema_root(Sema *s);
void sema_free(Sema *s);

#endif /* GALE_SEMA_H */
