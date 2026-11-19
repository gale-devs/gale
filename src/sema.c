#include "sema.h"

#include <limits.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common.h"

struct Sema {
    Arena *arena;
    TyCtx tys;
    StructInfo *root;
    SemaError *err;
    jmp_buf jb;
    const char *cur_file; /* file of the declaration being processed, for error messages */

    /* state while checking a function body */
    const StructInfo *cur_scope; /* the struct that contains the function */
    Ty *cur_ret;                 /* its return type */
    Vec locals;                  /* Sym*: parameters and variables in scope, innermost last */
};

/* printf helper for a Str: "%.*s" with SF(str) */
#define SF(x) (int)(x).len, (x).ptr

/* ---- plumbing ---- */

static NORETURN void fail(Sema *s, int line, int col, const char *fmt, ...) {
    va_list ap;
    snprintf(s->err->file, sizeof s->err->file, "%s", s->cur_file ? s->cur_file : "");
    va_start(ap, fmt);
    vsnprintf(s->err->msg, sizeof s->err->msg, fmt, ap);
    va_end(ap);
    s->err->line = line;
    s->err->col = col;
    longjmp(s->jb, 1);
}

Sema *sema_new(Arena *arena) {
    Sema *s = calloc(1, sizeof *s);
    if (!s) {
        fprintf(stderr, "gale: out of memory\n");
        exit(1);
    }
    s->arena = arena;
    ty_ctx_init(&s->tys, arena);
    vec_init(&s->locals, sizeof(Sym *));
    return s;
}

void sema_free(Sema *s) {
    free(s);
}

TyCtx *sema_types(Sema *s) {
    return &s->tys;
}

StructInfo *sema_root(Sema *s) {
    return s->root;
}

static int str_eq(Str a, Str b) {
    return a.len == b.len && memcmp(a.ptr, b.ptr, a.len) == 0;
}

/* "a.b.c" (allocated in the arena) */
static const char *path_str(Sema *s, const Path *p) {
    size_t len = 1, i;
    char *out, *w;

    for (i = 0; i < p->count; i++) {
        len += p->segs[i].len + 1;
    }
    out = arena_alloc(s->arena, len);
    w = out;
    for (i = 0; i < p->count; i++) {
        if (i) {
            *w++ = '.';
        }
        memcpy(w, p->segs[i].ptr, p->segs[i].len);
        w += p->segs[i].len;
    }
    return out;
}

/* Value of an integer literal (decimal, 0x, 0b, with '_' separators). Returns 0 if
 * it does not fit in 64 bits. */
static int parse_int(Str t, unsigned long long *out) {
    unsigned long long v = 0;
    unsigned base = 10;
    size_t i = 0;

    if (t.len > 2 && t.ptr[0] == '0' && (t.ptr[1] == 'x' || t.ptr[1] == 'X')) {
        base = 16;
        i = 2;
    } else if (t.len > 2 && t.ptr[0] == '0' && (t.ptr[1] == 'b' || t.ptr[1] == 'B')) {
        base = 2;
        i = 2;
    }
    for (; i < t.len; i++) {
        char c = t.ptr[i];
        unsigned d;

        if (c == '_') {
            continue;
        }
        if (c >= '0' && c <= '9') {
            d = (unsigned)(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            d = (unsigned)(c - 'a') + 10;
        } else if (c >= 'A' && c <= 'F') {
            d = (unsigned)(c - 'A') + 10;
        } else {
            return 0;
        }
        if (d >= base || v > (ULLONG_MAX - d) / base) {
            return 0;
        }
        v = v * base + d;
    }
    *out = v;
    return 1;
}

/* ---- scopes ---- */

static Sym *find_member(const StructInfo *si, Str name) {
    size_t i;
    for (i = 0; i < si->nmembers; i++) {
        if (str_eq(si->members[i]->name, name)) {
            return si->members[i];
        }
    }
    return NULL;
}

/* The member called `name` of `scope` or of the nearest enclosing struct that has one. */
static Sym *lookup_lexical(const StructInfo *scope, Str name) {
    for (; scope; scope = scope->parent) {
        Sym *m = find_member(scope, name);
        if (m) {
            return m;
        }
    }
    return NULL;
}

static int inside(const StructInfo *cur, const StructInfo *outer) {
    for (; cur; cur = cur->parent) {
        if (cur == outer) {
            return 1;
        }
    }
    return 0;
}

/* A member without `export` can only be used from inside the struct that declares it. */
static void check_visible(Sema *s, const StructInfo *scope, const StructInfo *owner, const Sym *m,
                          int line, int col) {
    if (m->item->exported || inside(scope, owner)) {
        return;
    }
    if (owner->name.len) {
        fail(s, line, col, "'%.*s' is private to '%.*s' (it is not exported)", SF(m->name),
             SF(owner->name));
    }
    fail(s, line, col, "'%.*s' is private (it is not exported)", SF(m->name));
}

/* ---- declarations ---- */

static StructInfo *declare_body(Sema *s, Body *body, StructInfo *parent, Item *item) {
    StructInfo *si = arena_alloc(s->arena, sizeof(StructInfo));
    Vec members;
    Sym **seen;
    size_t i, j;

    si->parent = parent;
    si->item = item;
    if (item) {
        s->cur_file = item->file;
        si->name = item->u.struct_.name;
        si->ty = ty_new_struct(&s->tys, si->name.ptr, si->name.len, si);
    }

    si->nfields = body->nfields;
    si->fields = arena_alloc(s->arena, (body->nfields + 1) * sizeof(FieldInfo));
    for (i = 0; i < body->nfields; i++) {
        const Field *f = body->fields[i];
        for (j = 0; j < i; j++) {
            if (str_eq(si->fields[j].name, f->name)) {
                fail(s, f->line, f->col, "field '%.*s' is declared twice", SF(f->name));
            }
        }
        si->fields[i].name = f->name;
        si->fields[i].line = f->line;
        si->fields[i].col = f->col;
    }

    vec_init(&members, sizeof(Sym *));
    for (i = 0; i < body->nitems; i++) {
        Item *it = body->items[i];
        Sym *sy;
        Str name;

        s->cur_file = it->file;
        if (it->kind == ITEM_UNFOLD) {
            fail(s, it->line, it->col, "'unfold' must be expanded before type checking");
        }
        name = it->kind == ITEM_FUNC ? it->u.func.name : it->u.struct_.name;
        seen = members.data;
        for (j = 0; j < members.len; j++) {
            if (str_eq(seen[j]->name, name)) {
                fail(s, it->line, it->col, "'%.*s' is already defined", SF(name));
            }
        }
        for (j = 0; j < si->nfields; j++) {
            if (str_eq(si->fields[j].name, name)) {
                fail(s, it->line, it->col, "'%.*s' is both a field and a member", SF(name));
            }
        }
        sy = arena_alloc(s->arena, sizeof(Sym));
        sy->kind = it->kind == ITEM_FUNC ? SYM_FUNC : SYM_STRUCT;
        sy->name = name;
        sy->item = it;
        sy->owner = si;
        it->sym = sy;
        vec_push(s->arena, &members, &sy);
        if (it->kind == ITEM_STRUCT) {
            sy->strukt = declare_body(s, &it->u.struct_.body, si, it);
            sy->ty = sy->strukt->ty;
        }
    }
    si->members = members.data;
    si->nmembers = members.len;
    return si;
}

/* ---- type resolution ---- */

static Ty *resolve_type(Sema *s, const StructInfo *scope, Type *t);

/* Like resolve_type, but `void` is not allowed (fields, parameters, elements). */
static Ty *resolve_value_type(Sema *s, const StructInfo *scope, Type *t, const char *what) {
    Ty *ty = resolve_type(s, scope, t);
    if (ty == s->tys.void_) {
        fail(s, t->line, t->col, "%s cannot have type void", what);
    }
    return ty;
}

/* a.b.c as members: the first segment is looked up in the enclosing structs, the rest
 * through struct members (which must be exported unless we are inside). Returns NULL
 * if the first segment is not a member in scope. */
static Sym *lookup_path(Sema *s, const StructInfo *scope, const Path *p, int line, int col) {
    Sym *sy = lookup_lexical(scope, p->segs[0]);
    size_t i;

    if (!sy) {
        return NULL;
    }
    for (i = 1; i < p->count; i++) {
        Sym *m;
        if (sy->kind != SYM_STRUCT) {
            fail(s, line, col, "'%.*s' is not a struct, so it has no member '%.*s'", SF(sy->name),
                 SF(p->segs[i]));
        }
        m = find_member(sy->strukt, p->segs[i]);
        if (!m) {
            fail(s, line, col, "'%.*s' has no member '%.*s'", SF(sy->name), SF(p->segs[i]));
        }
        check_visible(s, scope, sy->strukt, m, line, col);
        sy = m;
    }
    return sy;
}

static Ty *resolve_path_type(Sema *s, const StructInfo *scope, Type *t) {
    const Path *p = &t->u.path;
    Sym *sy = lookup_path(s, scope, p, t->line, t->col);

    if (!sy) {
        Ty *b = p->count == 1 ? ty_builtin(&s->tys, p->segs[0].ptr, p->segs[0].len) : NULL;
        if (b) {
            return b;
        }
        fail(s, t->line, t->col, "unknown type '%s'", path_str(s, p));
    }
    if (sy->kind != SYM_STRUCT) {
        fail(s, t->line, t->col, "'%s' is not a type", path_str(s, p));
    }
    return sy->ty;
}

static Ty *resolve_type(Sema *s, const StructInfo *scope, Type *t) {
    Ty *ty;

    switch (t->kind) {
    case TYPE_PATH:
        ty = resolve_path_type(s, scope, t);
        break;
    case TYPE_PTR:
        ty = ty_ptr(&s->tys, resolve_value_type(s, scope, t->u.elem, "a pointer target"));
        break;
    case TYPE_OPTIONAL:
        ty = ty_optional(&s->tys, resolve_value_type(s, scope, t->u.elem, "an optional"));
        break;
    case TYPE_SLICE:
        ty = ty_slice(&s->tys, resolve_value_type(s, scope, t->u.elem, "a slice element"));
        break;
    case TYPE_ERRUNION:
        ty = ty_errunion(&s->tys, resolve_type(s, scope, t->u.elem));
        break;
    case TYPE_ARRAY: {
        const Expr *len = t->u.array.len;
        unsigned long long n;
        if (len->kind != EXPR_INT) {
            fail(s, len->line, len->col, "an array length must be an integer literal");
        }
        if (!parse_int(len->u.lit, &n) || n > (unsigned long long)(~(size_t)0 >> 1)) {
            fail(s, len->line, len->col, "array length is too large");
        }
        ty = ty_array(&s->tys, resolve_value_type(s, scope, t->u.array.elem, "an array element"),
                      (size_t)n);
        break;
    }
    default:
        fail(s, t->line, t->col, "unsupported type");
    }
    t->sem = ty;
    return ty;
}

static void resolve_func(Sema *s, const StructInfo *scope, Sym *sy) {
    Item *it = sy->item;
    size_t n = it->u.func.nparams, i, j;
    Ty **params = arena_alloc(s->arena, (n + 1) * sizeof(Ty *));
    Ty *ret;

    for (i = 0; i < n; i++) {
        const Param *p = it->u.func.params[i];
        for (j = 0; j < i; j++) {
            if (str_eq(it->u.func.params[j]->name, p->name)) {
                fail(s, p->line, p->col, "parameter '%.*s' is declared twice", SF(p->name));
            }
        }
        params[i] = resolve_value_type(s, scope, it->u.func.params[i]->type, "a parameter");
    }
    ret = it->u.func.ret ? resolve_type(s, scope, it->u.func.ret) : s->tys.void_;
    sy->ty = ty_func(&s->tys, params, n, ret);
}

static void resolve_decls(Sema *s, StructInfo *si) {
    size_t i;

    for (i = 0; i < si->nfields; i++) {
        s->cur_file = si->item->file;
        si->fields[i].ty = resolve_value_type(s, si, si->item->u.struct_.body.fields[i]->type, "a field");
    }
    for (i = 0; i < si->nmembers; i++) {
        Sym *m = si->members[i];
        s->cur_file = m->item->file;
        if (m->kind == SYM_STRUCT) {
            resolve_decls(s, m->strukt);
        } else {
            resolve_func(s, si, m);
        }
    }
}

/* ---- recursive structs ---- */

static void visit_struct(Sema *s, StructInfo *si);

/* Types that hold their element inline: a struct cannot contain itself that way. */
static void visit_value_type(Sema *s, const Ty *t) {
    switch (t->kind) {
    case TY_ARRAY:
    case TY_OPTIONAL:
    case TY_ERRUNION:
        visit_value_type(s, t->elem);
        break;
    case TY_STRUCT:
        visit_struct(s, t->strukt);
        break;
    default:
        break;
    }
}

static void visit_struct(Sema *s, StructInfo *si) {
    size_t i;

    if (si->state == 2) {
        return;
    }
    if (si->state == 1) {
        s->cur_file = si->item->file;
        fail(s, si->item->line, si->item->col, "struct '%.*s' contains itself (use a pointer)",
             SF(si->name));
    }
    si->state = 1;
    for (i = 0; i < si->nfields; i++) {
        visit_value_type(s, si->fields[i].ty);
    }
    si->state = 2;
}

static void visit_all(Sema *s, StructInfo *si) {
    size_t i;

    if (si->item) {
        visit_struct(s, si);
    }
    for (i = 0; i < si->nmembers; i++) {
        if (si->members[i]->kind == SYM_STRUCT) {
            visit_all(s, si->members[i]->strukt);
        }
    }
}

/* ---- locals ---- */

static Sym *find_local(Sema *s, Str name) {
    Sym **l = s->locals.data;
    size_t i;

    for (i = s->locals.len; i-- > 0;) {
        if (str_eq(l[i]->name, name)) {
            return l[i];
        }
    }
    return NULL;
}

/* A local may not reuse the name of another local or parameter that is in scope. */
static Sym *declare_local(Sema *s, Str name, Ty *ty, int is_mutable, int is_param, int line, int col) {
    Sym *sy;

    if (find_local(s, name)) {
        fail(s, line, col, "'%.*s' is already declared", SF(name));
    }
    sy = arena_alloc(s->arena, sizeof(Sym));
    sy->kind = SYM_LOCAL;
    sy->name = name;
    sy->ty = ty;
    sy->is_mutable = is_mutable;
    sy->is_param = is_param;
    vec_push(s->arena, &s->locals, &sy);
    return sy;
}

/* ---- expressions ---- */

static Ty *check_expr(Sema *s, Expr *e, Ty *expected);

static const char *op_str(TokenKind k) {
    switch (k) {
    case TOK_PLUS: return "+";
    case TOK_MINUS: return "-";
    case TOK_STAR: return "*";
    case TOK_SLASH: return "/";
    case TOK_PERCENT: return "%";
    case TOK_AMP: return "&";
    case TOK_PIPE: return "|";
    case TOK_CARET: return "^";
    case TOK_TILDE: return "~";
    case TOK_BANG: return "!";
    case TOK_ANDAND: return "&&";
    case TOK_OROR: return "||";
    case TOK_SHL: return "<<";
    case TOK_SHR: return ">>";
    case TOK_EQ: return "==";
    case TOK_NE: return "!=";
    case TOK_LT: return "<";
    case TOK_LE: return "<=";
    case TOK_GT: return ">";
    case TOK_GE: return ">=";
    default: return "?";
    }
}

/* A number written in the source: its type is taken from the context. */
static int is_literal(const Expr *e) {
    if (e->kind == EXPR_INT || e->kind == EXPR_FLOAT) {
        return 1;
    }
    return e->kind == EXPR_UNARY && e->u.unary.op == TOK_MINUS && is_literal(e->u.unary.operand);
}

static void check_int_range(Sema *s, const Expr *lit, const Ty *ty, int neg, unsigned long long mag) {
    unsigned long long limit;

    if (ty->is_signed) {
        limit = 1ULL << (ty->bits - 1);
        if (neg ? mag > limit : mag > limit - 1) {
            fail(s, lit->line, lit->col, "the value does not fit in %s", ty_str(&s->tys, ty));
        }
    } else {
        limit = ty->bits >= 64 ? ULLONG_MAX : (1ULL << ty->bits) - 1;
        if ((neg && mag != 0) || mag > limit) {
            fail(s, lit->line, lit->col, "the value does not fit in %s", ty_str(&s->tys, ty));
        }
    }
}

/* An integer literal takes the expected integer or float type, otherwise i32. */
static Ty *check_int_literal(Sema *s, Expr *e, Ty *expected, int neg) {
    unsigned long long mag;
    Ty *ty;

    if (!parse_int(e->u.lit, &mag)) {
        fail(s, e->line, e->col, "the integer literal is too large");
    }
    ty = (expected && ty_is_numeric(expected)) ? expected : s->tys.i32;
    if (ty_is_integer(ty)) {
        check_int_range(s, e, ty, neg, mag);
    }
    e->ty = ty;
    return ty;
}

/* Checks both operands of a binary operator. A literal operand takes its type from
 * the other side, so `x + 1` works for any integer type of x. */
static void check_operands(Sema *s, Expr *l, Expr *r, Ty *expected, Ty **lt, Ty **rt) {
    if (is_literal(l) && !is_literal(r)) {
        *rt = check_expr(s, r, expected);
        *lt = check_expr(s, l, *rt);
    } else {
        *lt = check_expr(s, l, expected);
        *rt = check_expr(s, r, *lt);
    }
}

static Ty *check_binary(Sema *s, Expr *e, Ty *expected) {
    TokenKind op = e->u.binary.op;
    Expr *l = e->u.binary.lhs, *r = e->u.binary.rhs;
    Ty *lt, *rt;

    switch (op) {
    case TOK_ANDAND:
    case TOK_OROR:
        lt = check_expr(s, l, s->tys.bool_);
        rt = check_expr(s, r, s->tys.bool_);
        if (lt != s->tys.bool_ || rt != s->tys.bool_) {
            fail(s, e->line, e->col, "'%s' needs bool operands, found %s and %s", op_str(op),
                 ty_str(&s->tys, lt), ty_str(&s->tys, rt));
        }
        return s->tys.bool_;

    case TOK_EQ:
    case TOK_NE:
    case TOK_LT:
    case TOK_LE:
    case TOK_GT:
    case TOK_GE:
        check_operands(s, l, r, NULL, &lt, &rt);
        if (lt != rt) {
            fail(s, e->line, e->col, "cannot compare %s with %s", ty_str(&s->tys, lt),
                 ty_str(&s->tys, rt));
        }
        if (op == TOK_EQ || op == TOK_NE) {
            if (!ty_is_numeric(lt) && lt->kind != TY_BOOL && lt->kind != TY_PTR) {
                fail(s, e->line, e->col, "'%s' does not work on %s", op_str(op), ty_str(&s->tys, lt));
            }
        } else if (!ty_is_numeric(lt)) {
            fail(s, e->line, e->col, "'%s' does not work on %s", op_str(op), ty_str(&s->tys, lt));
        }
        return s->tys.bool_;

    case TOK_SHL:
    case TOK_SHR:
        lt = check_expr(s, l, expected);
        rt = check_expr(s, r, lt);
        if (!ty_is_integer(lt) || !ty_is_integer(rt)) {
            fail(s, e->line, e->col, "'%s' needs integer operands, found %s and %s", op_str(op),
                 ty_str(&s->tys, lt), ty_str(&s->tys, rt));
        }
        return lt;

    case TOK_AMP:
    case TOK_PIPE:
    case TOK_CARET:
    case TOK_PERCENT:
        check_operands(s, l, r, expected, &lt, &rt);
        if (lt != rt) {
            fail(s, e->line, e->col, "mismatched types for '%s': %s and %s", op_str(op),
                 ty_str(&s->tys, lt), ty_str(&s->tys, rt));
        }
        if (!ty_is_integer(lt)) {
            fail(s, e->line, e->col, "'%s' needs integer operands, found %s", op_str(op),
                 ty_str(&s->tys, lt));
        }
        return lt;

    default: /* + - * / */
        check_operands(s, l, r, expected, &lt, &rt);
        if (lt != rt) {
            fail(s, e->line, e->col, "mismatched types for '%s': %s and %s", op_str(op),
                 ty_str(&s->tys, lt), ty_str(&s->tys, rt));
        }
        if (!ty_is_numeric(lt)) {
            fail(s, e->line, e->col, "'%s' needs numeric operands, found %s", op_str(op),
                 ty_str(&s->tys, lt));
        }
        return lt;
    }
}

static NORETURN void fail_mismatch(Sema *s, int line, int col, const Ty *want, const Ty *got) {
    fail(s, line, col, "expected %s, found %s", ty_str(&s->tys, want), ty_str(&s->tys, got));
}

/* Can `e` (already checked) be assigned to or have its address taken? */
static int is_lvalue(const Expr *e) {
    switch (e->kind) {
    case EXPR_IDENT:
        return e->sym && e->sym->kind == SYM_LOCAL;
    case EXPR_FIELD:
        if (e->sym) {
            return 0; /* a member of a struct, not a field of a value */
        }
        return e->u.field.base->ty->kind == TY_PTR || is_lvalue(e->u.field.base);
    case EXPR_INDEX:
        return e->u.index.base->ty->kind != TY_ARRAY || is_lvalue(e->u.index.base);
    case EXPR_UNARY:
        return e->u.unary.op == TOK_STAR;
    default:
        return 0;
    }
}

/* If `e` is a name or a.b.c that refers to a member (not to a local variable or a
 * field of a value), returns it; otherwise NULL. */
static Sym *try_resolve_path(Sema *s, Expr *e) {
    if (e->kind == EXPR_IDENT) {
        Sym *sy;
        if (find_local(s, e->u.lit)) {
            return NULL;
        }
        sy = lookup_lexical(s->cur_scope, e->u.lit);
        if (!sy) {
            fail(s, e->line, e->col, "unknown name '%.*s'", SF(e->u.lit));
        }
        e->sym = sy;
        return sy;
    }
    if (e->kind == EXPR_FIELD) {
        Sym *b = try_resolve_path(s, e->u.field.base);
        Sym *m;

        if (!b) {
            return NULL;
        }
        if (b->kind != SYM_STRUCT) {
            fail(s, e->line, e->col, "'%.*s' is a function and has no members", SF(b->name));
        }
        m = find_member(b->strukt, e->u.field.name);
        if (!m) {
            fail(s, e->line, e->col, "'%.*s' has no member '%.*s'", SF(b->name), SF(e->u.field.name));
        }
        check_visible(s, s->cur_scope, b->strukt, m, e->line, e->col);
        e->sym = m;
        return m;
    }
    return NULL;
}

/* `a.b` where a is a value (a struct or a pointer to one) or a path to a member. */
static Ty *check_field(Sema *s, Expr *e) {
    Sym *sy = try_resolve_path(s, e);
    Ty *bt, *st;
    const StructInfo *si;
    size_t i;

    if (sy) {
        if (sy->kind == SYM_STRUCT) {
            fail(s, e->line, e->col, "'%.*s' is a type, not a value", SF(sy->name));
        }
        fail(s, e->line, e->col, "'%.*s' is a function; call it with ()", SF(sy->name));
    }
    bt = check_expr(s, e->u.field.base, NULL);
    st = bt->kind == TY_PTR ? bt->elem : bt;
    if (st->kind != TY_STRUCT) {
        fail(s, e->line, e->col, "%s has no fields", ty_str(&s->tys, bt));
    }
    si = st->strukt;
    for (i = 0; i < si->nfields; i++) {
        if (str_eq(si->fields[i].name, e->u.field.name)) {
            return si->fields[i].ty;
        }
    }
    if (find_member(si, e->u.field.name)) {
        fail(s, e->line, e->col, "'%.*s' is not a field; call it with ()", SF(e->u.field.name));
    }
    fail(s, e->line, e->col, "%s has no field '%.*s'", ty_str(&s->tys, st), SF(e->u.field.name));
}

/* recv.f(args): f is a function of the receiver's struct whose first parameter takes
 * the receiver (S or *S). Records how the receiver is passed. */
static Sym *resolve_method(Sema *s, Expr *call) {
    Expr *callee = call->u.call.callee;
    Expr *base = callee->u.field.base;
    Ty *bt = check_expr(s, base, NULL);
    Ty *st = bt->kind == TY_PTR ? bt->elem : bt;
    Sym *m;
    Ty *p0;

    if (st->kind != TY_STRUCT) {
        fail(s, callee->line, callee->col, "%s has no methods", ty_str(&s->tys, bt));
    }
    m = find_member(st->strukt, callee->u.field.name);
    if (!m || m->kind != SYM_FUNC) {
        fail(s, callee->line, callee->col, "%s has no method '%.*s'", ty_str(&s->tys, st),
             SF(callee->u.field.name));
    }
    check_visible(s, s->cur_scope, st->strukt, m, callee->line, callee->col);
    if (m->ty->nparams == 0) {
        fail(s, callee->line, callee->col, "'%.*s' takes no receiver (no parameters)", SF(m->name));
    }
    p0 = m->ty->params[0];
    if (p0 == bt) {
        call->u.call.recv_adjust = 0;
    } else if (p0->kind == TY_PTR && p0->elem == st && bt == st) {
        if (!is_lvalue(base)) {
            fail(s, base->line, base->col, "cannot take the address of this receiver");
        }
        call->u.call.recv_adjust = 1;
    } else if (p0 == st && bt->kind == TY_PTR) {
        call->u.call.recv_adjust = 2;
    } else {
        fail(s, callee->line, callee->col,
             "the first parameter of '%.*s' is %s, which does not take a receiver of type %s",
             SF(m->name), ty_str(&s->tys, p0), ty_str(&s->tys, bt));
    }
    call->u.call.is_method = 1;
    return m;
}

static Ty *check_call(Sema *s, Expr *e) {
    Expr *callee = e->u.call.callee;
    Sym *fn = NULL;
    Ty *ft;
    size_t first = 0, expect, i;

    if (callee->kind == EXPR_IDENT || callee->kind == EXPR_FIELD) {
        Sym *sy = try_resolve_path(s, callee);
        if (sy) {
            if (sy->kind != SYM_FUNC) {
                fail(s, callee->line, callee->col, "'%.*s' is a type, not a function", SF(sy->name));
            }
            fn = sy;
        } else if (callee->kind == EXPR_FIELD) {
            fn = resolve_method(s, e);
            first = 1;
        } else {
            fail(s, callee->line, callee->col, "'%.*s' is a variable, not a function",
                 SF(callee->u.lit));
        }
    } else {
        fail(s, callee->line, callee->col, "this expression is not a function");
    }

    ft = fn->ty;
    callee->sym = fn;
    callee->ty = ft;
    expect = ft->nparams - first;
    if (e->u.call.nargs != expect) {
        fail(s, e->line, e->col, "'%.*s' takes %lu argument%s, found %lu", SF(fn->name),
             (unsigned long)expect, expect == 1 ? "" : "s", (unsigned long)e->u.call.nargs);
    }
    for (i = 0; i < e->u.call.nargs; i++) {
        Expr *a = e->u.call.args[i];
        Ty *pt = ft->params[i + first];
        Ty *at = check_expr(s, a, pt);
        if (!ty_assignable(at, pt)) {
            fail(s, a->line, a->col, "argument %lu of '%.*s': expected %s, found %s",
                 (unsigned long)(i + 1), SF(fn->name), ty_str(&s->tys, pt), ty_str(&s->tys, at));
        }
    }
    e->sym = fn;
    return ft->ret;
}

static Ty *check_index(Sema *s, Expr *e) {
    Ty *bt = check_expr(s, e->u.index.base, NULL);
    Ty *it = check_expr(s, e->u.index.index, s->tys.usize);

    if (!ty_is_integer(it)) {
        fail(s, e->u.index.index->line, e->u.index.index->col, "an index must be an integer, found %s",
             ty_str(&s->tys, it));
    }
    if (bt->kind != TY_ARRAY && bt->kind != TY_SLICE && bt->kind != TY_PTR) {
        fail(s, e->line, e->col, "cannot index %s", ty_str(&s->tys, bt));
    }
    return bt->elem;
}

static Ty *check_struct_lit(Sema *s, Expr *e) {
    const Path *path = &e->u.struct_lit.path;
    Sym *sy = lookup_path(s, s->cur_scope, path, e->line, e->col);
    const StructInfo *si;
    unsigned char *given;
    size_t i, j;

    if (!sy) {
        fail(s, e->line, e->col, "unknown type '%s'", path_str(s, path));
    }
    if (sy->kind != SYM_STRUCT) {
        fail(s, e->line, e->col, "'%s' is not a type", path_str(s, path));
    }
    si = sy->strukt;
    given = arena_alloc(s->arena, si->nfields + 1);
    for (i = 0; i < e->u.struct_lit.nfields; i++) {
        FieldInit *fi = e->u.struct_lit.fields[i];
        Ty *vt;

        for (j = 0; j < si->nfields && !str_eq(si->fields[j].name, fi->name); j++) {
        }
        if (j == si->nfields) {
            fail(s, fi->line, fi->col, "%.*s has no field '%.*s'", SF(si->name), SF(fi->name));
        }
        if (given[j]) {
            fail(s, fi->line, fi->col, "field '%.*s' is given twice", SF(fi->name));
        }
        given[j] = 1;
        vt = check_expr(s, fi->value, si->fields[j].ty);
        if (!ty_assignable(vt, si->fields[j].ty)) {
            fail_mismatch(s, fi->value->line, fi->value->col, si->fields[j].ty, vt);
        }
    }
    for (j = 0; j < si->nfields; j++) {
        if (!given[j]) {
            fail(s, e->line, e->col, "missing field '%.*s'", SF(si->fields[j].name));
        }
    }
    e->sym = sy;
    return sy->ty;
}

static Ty *check_unary(Sema *s, Expr *e, Ty *expected) {
    Expr *x = e->u.unary.operand;
    Ty *t;

    switch (e->u.unary.op) {
    case TOK_MINUS:
        if (x->kind == EXPR_INT) {
            t = check_int_literal(s, x, expected, 1);
            return t;
        }
        t = check_expr(s, x, expected);
        if (!ty_is_numeric(t)) {
            fail(s, e->line, e->col, "cannot negate %s", ty_str(&s->tys, t));
        }
        if (ty_is_integer(t) && !t->is_signed) {
            fail(s, e->line, e->col, "cannot negate the unsigned type %s", ty_str(&s->tys, t));
        }
        return t;
    case TOK_BANG:
        t = check_expr(s, x, s->tys.bool_);
        if (t != s->tys.bool_) {
            fail(s, e->line, e->col, "'!' needs a bool, found %s", ty_str(&s->tys, t));
        }
        return t;
    case TOK_TILDE:
        t = check_expr(s, x, expected);
        if (!ty_is_integer(t)) {
            fail(s, e->line, e->col, "'~' needs an integer, found %s", ty_str(&s->tys, t));
        }
        return t;
    case TOK_STAR:
        t = check_expr(s, x, NULL);
        if (t->kind != TY_PTR) {
            fail(s, e->line, e->col, "cannot dereference %s", ty_str(&s->tys, t));
        }
        return t->elem;
    case TOK_AMP:
        t = check_expr(s, x, expected && expected->kind == TY_PTR ? expected->elem : NULL);
        if (!is_lvalue(x)) {
            fail(s, e->line, e->col, "cannot take the address of this expression");
        }
        return ty_ptr(&s->tys, t);
    case TOK_KW_TRY:
        if (s->cur_ret->kind != TY_ERRUNION) {
            fail(s, e->line, e->col, "'try' can only be used in a function that returns an error union");
        }
        t = check_expr(s, x, NULL);
        if (t->kind != TY_ERRUNION) {
            fail(s, e->line, e->col, "'try' needs an error union, found %s", ty_str(&s->tys, t));
        }
        return t->elem;
    default:
        fail(s, e->line, e->col, "this operator is not supported yet");
    }
}

/* Which conversions `as` allows. */
static int castable(const Ty *from, const Ty *to) {
    if (from == to) {
        return 1;
    }
    if (ty_is_numeric(from) && ty_is_numeric(to)) {
        return 1;
    }
    if (from->kind == TY_BOOL && ty_is_integer(to)) {
        return 1;
    }
    if (from->kind == TY_PTR && to->kind == TY_PTR) {
        return 1;
    }
    if ((from->kind == TY_PTR && ty_is_integer(to)) || (ty_is_integer(from) && to->kind == TY_PTR)) {
        return 1;
    }
    return 0;
}

static Ty *check_cast(Sema *s, Expr *e) {
    Ty *to = resolve_value_type(s, s->cur_scope, e->u.cast.type, "a cast target");
    Ty *from = check_expr(s, e->u.cast.operand, NULL);

    if (!castable(from, to)) {
        fail(s, e->line, e->col, "cannot cast %s to %s", ty_str(&s->tys, from), ty_str(&s->tys, to));
    }
    return to;
}

static Ty *check_ident(Sema *s, Expr *e) {
    Sym *sy = find_local(s, e->u.lit);

    if (!sy) {
        sy = lookup_lexical(s->cur_scope, e->u.lit);
    }
    if (!sy) {
        fail(s, e->line, e->col, "unknown name '%.*s'", SF(e->u.lit));
    }
    e->sym = sy;
    if (sy->kind == SYM_FUNC) {
        fail(s, e->line, e->col, "'%.*s' is a function; call it with ()", SF(e->u.lit));
    }
    if (sy->kind == SYM_STRUCT) {
        fail(s, e->line, e->col, "'%.*s' is a type, not a value", SF(e->u.lit));
    }
    return sy->ty;
}

/* Can the place `e` (already checked, and an lvalue) be written to? Variables must be
 * declared with `var`; through a pointer or a slice anything can be written. */
static int is_mutable(const Expr *e) {
    switch (e->kind) {
    case EXPR_IDENT:
        return e->sym && e->sym->kind == SYM_LOCAL && e->sym->is_mutable;
    case EXPR_FIELD:
        return e->u.field.base->ty->kind == TY_PTR || is_mutable(e->u.field.base);
    case EXPR_INDEX:
        return e->u.index.base->ty->kind != TY_ARRAY || is_mutable(e->u.index.base);
    case EXPR_UNARY:
        return e->u.unary.op == TOK_STAR;
    default:
        return 0;
    }
}

static Ty *check_assign(Sema *s, Expr *e) {
    Expr *l = e->u.binary.lhs, *r = e->u.binary.rhs;
    Ty *lt = check_expr(s, l, NULL);
    Ty *rt;

    if (!is_lvalue(l)) {
        fail(s, l->line, l->col, "cannot assign to this expression");
    }
    if (!is_mutable(l)) {
        if (l->kind == EXPR_IDENT) {
            fail(s, l->line, l->col, "cannot assign to '%.*s': %s", SF(l->u.lit),
                 l->sym->is_param ? "parameters are immutable" : "it is declared with let (use var)");
        }
        fail(s, l->line, l->col, "cannot assign to this value: it is not mutable");
    }
    rt = check_expr(s, r, lt);
    if (!ty_assignable(rt, lt)) {
        fail_mismatch(s, r->line, r->col, lt, rt);
    }
    return s->tys.void_;
}

/* The common type of the two branches of an if/else. A branch that never finishes
 * (it returns) takes the type of the other one. */
static Ty *unify_branches(Sema *s, Expr *else_expr, Ty *a, Ty *b) {
    if (a == s->tys.never) {
        return b;
    }
    if (b == s->tys.never || ty_assignable(b, a)) {
        return a;
    }
    if (ty_assignable(a, b)) {
        return b;
    }
    fail(s, else_expr->line, else_expr->col, "if and else have different types: %s and %s",
         ty_str(&s->tys, a), ty_str(&s->tys, b));
}

static void check_condition(Sema *s, Expr *cond) {
    Ty *t = check_expr(s, cond, s->tys.bool_);
    if (t != s->tys.bool_) {
        fail_mismatch(s, cond->line, cond->col, s->tys.bool_, t);
    }
}

static Ty *check_if(Sema *s, Expr *e, Ty *expected) {
    Ty *then_ty, *else_ty;

    check_condition(s, e->u.if_.cond);
    then_ty = check_expr(s, e->u.if_.then_block, expected);
    if (!e->u.if_.else_expr) {
        return s->tys.void_; /* without else the value of the branch is discarded */
    }
    else_ty = check_expr(s, e->u.if_.else_expr,
                         expected ? expected : (then_ty == s->tys.never ? NULL : then_ty));
    return unify_branches(s, e->u.if_.else_expr, then_ty, else_ty);
}

static Ty *check_while(Sema *s, Expr *e) {
    check_condition(s, e->u.while_.cond);
    check_expr(s, e->u.while_.body, NULL);
    return s->tys.void_;
}

static void check_return(Sema *s, Stmt *st) {
    Ty *ret = s->cur_ret;

    if (st->expr) {
        Ty *t;
        if (ret == s->tys.void_) {
            fail(s, st->line, st->col, "this function does not return a value");
        }
        t = check_expr(s, st->expr, ret);
        if (!ty_assignable(t, ret)) {
            fail_mismatch(s, st->expr->line, st->expr->col, ret, t);
        }
    } else if (ret != s->tys.void_) {
        fail(s, st->line, st->col, "this function must return a value of type %s",
             ty_str(&s->tys, ret));
    }
}

static void check_decl(Sema *s, Stmt *st) {
    Ty *ty, *t;

    if (st->type) {
        ty = resolve_value_type(s, s->cur_scope, st->type, "a variable");
        t = check_expr(s, st->expr, ty);
        if (!ty_assignable(t, ty)) {
            fail_mismatch(s, st->expr->line, st->expr->col, ty, t);
        }
    } else {
        ty = check_expr(s, st->expr, NULL);
        if (ty == s->tys.void_ || ty == s->tys.never) {
            fail(s, st->expr->line, st->expr->col, "this expression has no value to store in a variable");
        }
    }
    /* declared after the initializer, so `let x = x` does not see itself */
    st->sym = declare_local(s, st->name, ty, st->kind == STMT_VAR, 0, st->line, st->col);
}

/* { stmts [tail] }. Variables declared inside end with the block. A block without a
 * tail is void, or `never` if its last statement is a return. */
static Ty *check_block(Sema *s, Expr *e, Ty *expected) {
    Block *b = e->u.block;
    size_t mark = s->locals.len, i;
    Ty *result;

    for (i = 0; i < b->nstmts; i++) {
        Stmt *st = b->stmts[i];

        switch (st->kind) {
        case STMT_LET:
        case STMT_VAR:
            check_decl(s, st);
            break;
        case STMT_DEFER:
            check_expr(s, st->expr, NULL);
            break;
        case STMT_RETURN:
            check_return(s, st);
            break;
        case STMT_EXPR:
            check_expr(s, st->expr, NULL);
            break;
        }
    }
    if (b->tail) {
        result = check_expr(s, b->tail, expected);
    } else if (b->nstmts && b->stmts[b->nstmts - 1]->kind == STMT_RETURN) {
        result = s->tys.never;
    } else {
        result = s->tys.void_;
    }
    s->locals.len = mark;
    return result;
}

/* Type of `e`; `expected` (may be NULL) is the type the context wants, used to type
 * number literals. Sets e->ty. */
static Ty *check_expr(Sema *s, Expr *e, Ty *expected) {
    Ty *t;

    switch (e->kind) {
    case EXPR_INT:
        t = check_int_literal(s, e, expected, 0);
        break;
    case EXPR_FLOAT:
        t = (expected && ty_is_float(expected)) ? expected : s->tys.f64;
        break;
    case EXPR_STRING:
        t = s->tys.str;
        break;
    case EXPR_BOOL:
        t = s->tys.bool_;
        break;
    case EXPR_IDENT:
        t = check_ident(s, e);
        break;
    case EXPR_UNARY:
        t = check_unary(s, e, expected);
        break;
    case EXPR_BINARY:
        t = check_binary(s, e, expected);
        break;
    case EXPR_CAST:
        t = check_cast(s, e);
        break;
    case EXPR_FIELD:
        t = check_field(s, e);
        break;
    case EXPR_CALL:
        t = check_call(s, e);
        break;
    case EXPR_INDEX:
        t = check_index(s, e);
        break;
    case EXPR_STRUCT_LIT:
        t = check_struct_lit(s, e);
        break;
    case EXPR_ASSIGN:
        t = check_assign(s, e);
        break;
    case EXPR_BLOCK:
        t = check_block(s, e, expected);
        break;
    case EXPR_IF:
        t = check_if(s, e, expected);
        break;
    case EXPR_WHILE:
        t = check_while(s, e);
        break;
    default:
        fail(s, e->line, e->col, "this kind of expression is not supported yet");
    }
    e->ty = t;
    return t;
}

/* ---- function bodies ---- */

static void check_func(Sema *s, const StructInfo *scope, Sym *sy) {
    Item *it = sy->item;
    Ty *ret = sy->ty->ret;
    Ty *t;
    size_t i;

    s->cur_file = it->file;
    s->cur_scope = scope;
    s->cur_ret = ret;
    s->locals.len = 0;
    for (i = 0; i < it->u.func.nparams; i++) {
        const Param *p = it->u.func.params[i];
        declare_local(s, p->name, sy->ty->params[i], 0, 1, p->line, p->col);
    }
    t = check_expr(s, it->u.func.body, ret == s->tys.void_ ? NULL : ret);
    /* a void function discards the value of its body */
    if (ret != s->tys.void_ && !ty_assignable(t, ret)) {
        fail(s, it->u.func.body->line, it->u.func.body->col,
             "the body has type %s, but the function returns %s", ty_str(&s->tys, t),
             ty_str(&s->tys, ret));
    }
}

static void check_bodies(Sema *s, const StructInfo *si) {
    size_t i;

    for (i = 0; i < si->nmembers; i++) {
        Sym *m = si->members[i];
        if (m->kind == SYM_FUNC) {
            check_func(s, si, m);
        } else {
            check_bodies(s, m->strukt);
        }
    }
}

/* ---- entry point ---- */

int sema_check(Sema *s, File *file, SemaError *err) {
    s->err = err;
    err->file[0] = '\0';
    err->msg[0] = '\0';
    err->line = 0;
    err->col = 0;
    s->cur_file = NULL;
    if (setjmp(s->jb)) {
        return 0;
    }
    s->root = declare_body(s, &file->body, NULL, NULL);
    resolve_decls(s, s->root);
    visit_all(s, s->root);
    check_bodies(s, s->root);
    return 1;
}
