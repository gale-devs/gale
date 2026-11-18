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

static Ty *resolve_path_type(Sema *s, const StructInfo *scope, Type *t) {
    const Path *p = &t->u.path;
    Sym *sy = lookup_lexical(scope, p->segs[0]);
    size_t i;

    if (!sy) {
        Ty *b = p->count == 1 ? ty_builtin(&s->tys, p->segs[0].ptr, p->segs[0].len) : NULL;
        if (b) {
            return b;
        }
        fail(s, t->line, t->col, "unknown type '%s'", path_str(s, p));
    }
    for (i = 1; i < p->count; i++) {
        Sym *m;
        if (sy->kind != SYM_STRUCT) {
            fail(s, t->line, t->col, "'%.*s' is not a struct, so it has no member '%.*s'",
                 SF(sy->name), SF(p->segs[i]));
        }
        m = find_member(sy->strukt, p->segs[i]);
        if (!m) {
            fail(s, t->line, t->col, "'%.*s' has no member '%.*s'", SF(sy->name), SF(p->segs[i]));
        }
        check_visible(s, scope, sy->strukt, m, t->line, t->col);
        sy = m;
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
        si->fields[i].ty = resolve_value_type(s, si, si->item->u.struct_.body.fields[i]->type, "a field");
    }
    for (i = 0; i < si->nmembers; i++) {
        Sym *m = si->members[i];
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
    err->msg[0] = '\0';
    err->line = 0;
    err->col = 0;
    if (setjmp(s->jb)) {
        return 0;
    }
    s->root = declare_body(s, &file->body, NULL, NULL);
    resolve_decls(s, s->root);
    visit_all(s, s->root);
    check_bodies(s, s->root);
    return 1;
}
