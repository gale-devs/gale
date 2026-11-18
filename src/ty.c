#include "ty.h"

#include <stdio.h>
#include <string.h>

static Ty *new_ty(TyCtx *c, TyKind kind) {
    Ty *t = arena_alloc(c->arena, sizeof(Ty));
    t->kind = kind;
    return t;
}

static Ty *scalar(TyCtx *c, TyKind kind, const char *name, int bits, int is_signed) {
    Ty *t = new_ty(c, kind);
    t->name = name;
    t->bits = bits;
    t->is_signed = is_signed;
    return t;
}

void ty_ctx_init(TyCtx *c, Arena *arena) {
    memset(c, 0, sizeof *c);
    c->arena = arena;
    vec_init(&c->composite, sizeof(Ty *));

    c->void_ = scalar(c, TY_VOID, "void", 0, 0);
    c->never = scalar(c, TY_NEVER, "never", 0, 0);
    c->bool_ = scalar(c, TY_BOOL, "bool", 1, 0);
    c->i8 = scalar(c, TY_INT, "i8", 8, 1);
    c->i16 = scalar(c, TY_INT, "i16", 16, 1);
    c->i32 = scalar(c, TY_INT, "i32", 32, 1);
    c->i64 = scalar(c, TY_INT, "i64", 64, 1);
    c->isize = scalar(c, TY_INT, "isize", 64, 1);
    c->u8 = scalar(c, TY_INT, "u8", 8, 0);
    c->u16 = scalar(c, TY_INT, "u16", 16, 0);
    c->u32 = scalar(c, TY_INT, "u32", 32, 0);
    c->u64 = scalar(c, TY_INT, "u64", 64, 0);
    c->usize = scalar(c, TY_INT, "usize", 64, 0);
    c->f32 = scalar(c, TY_FLOAT, "f32", 32, 1);
    c->f64 = scalar(c, TY_FLOAT, "f64", 64, 1);
    c->str = ty_slice(c, c->u8);
}

Ty *ty_builtin(TyCtx *c, const char *name, size_t len) {
    Ty *all[17];
    size_t i;

    all[0] = c->void_;
    all[1] = c->bool_;
    all[2] = c->i8;
    all[3] = c->i16;
    all[4] = c->i32;
    all[5] = c->i64;
    all[6] = c->isize;
    all[7] = c->u8;
    all[8] = c->u16;
    all[9] = c->u32;
    all[10] = c->u64;
    all[11] = c->usize;
    all[12] = c->f32;
    all[13] = c->f64;
    for (i = 0; i < 14; i++) {
        if (strlen(all[i]->name) == len && memcmp(all[i]->name, name, len) == 0) {
            return all[i];
        }
    }
    return NULL;
}

/* Finds or creates the interned composite type of this shape. */
static Ty *composite(TyCtx *c, TyKind kind, Ty *elem, size_t len) {
    size_t i;
    Ty *t;

    for (i = 0; i < c->composite.len; i++) {
        Ty *x = ((Ty **)c->composite.data)[i];
        if (x->kind == kind && x->elem == elem && x->len == len) {
            return x;
        }
    }
    t = new_ty(c, kind);
    t->elem = elem;
    t->len = len;
    vec_push(c->arena, &c->composite, &t);
    return t;
}

Ty *ty_ptr(TyCtx *c, Ty *elem) {
    return composite(c, TY_PTR, elem, 0);
}

Ty *ty_slice(TyCtx *c, Ty *elem) {
    return composite(c, TY_SLICE, elem, 0);
}

Ty *ty_array(TyCtx *c, Ty *elem, size_t len) {
    return composite(c, TY_ARRAY, elem, len);
}

Ty *ty_optional(TyCtx *c, Ty *elem) {
    return composite(c, TY_OPTIONAL, elem, 0);
}

Ty *ty_errunion(TyCtx *c, Ty *elem) {
    return composite(c, TY_ERRUNION, elem, 0);
}

Ty *ty_func(TyCtx *c, Ty **params, size_t nparams, Ty *ret) {
    size_t i;
    Ty *t;

    for (i = 0; i < c->composite.len; i++) {
        Ty *x = ((Ty **)c->composite.data)[i];
        if (x->kind == TY_FUNC && x->ret == ret && x->nparams == nparams &&
            (nparams == 0 || memcmp(x->params, params, nparams * sizeof(Ty *)) == 0)) {
            return x;
        }
    }
    t = new_ty(c, TY_FUNC);
    t->ret = ret;
    t->nparams = nparams;
    if (nparams) {
        t->params = arena_alloc(c->arena, nparams * sizeof(Ty *));
        memcpy(t->params, params, nparams * sizeof(Ty *));
    }
    vec_push(c->arena, &c->composite, &t);
    return t;
}

Ty *ty_new_struct(TyCtx *c, const char *name, size_t name_len, StructInfo *info) {
    Ty *t = new_ty(c, TY_STRUCT);
    char *n = arena_alloc(c->arena, name_len + 1);
    memcpy(n, name, name_len);
    t->name = n;
    t->strukt = info;
    return t;
}

int ty_is_integer(const Ty *t) {
    return t->kind == TY_INT;
}

int ty_is_float(const Ty *t) {
    return t->kind == TY_FLOAT;
}

int ty_is_numeric(const Ty *t) {
    return t->kind == TY_INT || t->kind == TY_FLOAT;
}

int ty_assignable(const Ty *from, const Ty *to) {
    if (from == to || from->kind == TY_NEVER) {
        return 1;
    }
    if (to->kind == TY_OPTIONAL || to->kind == TY_ERRUNION) {
        return ty_assignable(from, to->elem);
    }
    return 0;
}

static void fmt(const Ty *t, char *buf, size_t cap) {
    size_t used = strlen(buf);
    char tmp[40];

    if (used + 1 >= cap) {
        return;
    }
    switch (t->kind) {
    case TY_PTR:
        strncat(buf, "*", cap - used - 1);
        fmt(t->elem, buf, cap);
        break;
    case TY_OPTIONAL:
        strncat(buf, "?", cap - used - 1);
        fmt(t->elem, buf, cap);
        break;
    case TY_SLICE:
        strncat(buf, "[]", cap - used - 1);
        fmt(t->elem, buf, cap);
        break;
    case TY_ARRAY:
        snprintf(tmp, sizeof tmp, "[%lu]", (unsigned long)t->len);
        strncat(buf, tmp, cap - used - 1);
        fmt(t->elem, buf, cap);
        break;
    case TY_ERRUNION:
        fmt(t->elem, buf, cap);
        strncat(buf, "!", cap - strlen(buf) - 1);
        break;
    case TY_FUNC: {
        size_t i;
        strncat(buf, "func(", cap - used - 1);
        for (i = 0; i < t->nparams; i++) {
            if (i) {
                strncat(buf, ", ", cap - strlen(buf) - 1);
            }
            fmt(t->params[i], buf, cap);
        }
        strncat(buf, ") -> ", cap - strlen(buf) - 1);
        fmt(t->ret, buf, cap);
        break;
    }
    default:
        strncat(buf, t->name, cap - used - 1);
        break;
    }
}

const char *ty_str(TyCtx *c, const Ty *t) {
    char buf[256];
    char *out;

    buf[0] = '\0';
    fmt(t, buf, sizeof buf);
    out = arena_alloc(c->arena, strlen(buf) + 1);
    strcpy(out, buf);
    return out;
}
