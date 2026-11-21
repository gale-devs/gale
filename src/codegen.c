#include "codegen.h"

#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "common.h"

#define REG(r) opd_reg(r)
#define IMM(v) opd_imm(v)
#define MEM(r, d) opd_mem(r, d)

typedef enum {
    C_VOID,   /* no value */
    C_SCALAR, /* bool, integers, pointers: the value is in rax (canonical 64-bit form) */
    C_AGG     /* structs, arrays, slices: rax holds the address of the value */
} Class;

typedef struct {
    Sema *sema;
    TyCtx *tys;
    Arena *arena;
    Asm *as;
    CodegenError *err;
    jmp_buf jb;
    const char *cur_file;

    Vec worklist; /* Sym* of functions still to generate */

    /* the function being generated */
    Sym *fn;
    Ty *ret;
    long frame;        /* bytes of locals so far */
    long retptr_off;   /* [rbp+off]: where a struct result is written (struct returns) */
    int ret_label;
    Vec defers;        /* Expr*: pending `defer`s of the enclosing scopes */
    int in_defer;
} Gen;

static NORETURN void fail(Gen *g, int line, int col, const char *fmt, ...) {
    va_list ap;
    snprintf(g->err->file, sizeof g->err->file, "%s", g->cur_file ? g->cur_file : "");
    va_start(ap, fmt);
    vsnprintf(g->err->msg, sizeof g->err->msg, fmt, ap);
    va_end(ap);
    g->err->line = line;
    g->err->col = col;
    longjmp(g->jb, 1);
}

/* ---- types: classes, sizes, layout ---- */

static size_t align_up(size_t n, size_t a) {
    return (n + a - 1) / a * a;
}

static NORETURN void unsupported(Gen *g, int line, int col, const Ty *t) {
    fail(g, line, col, "the code generator does not support %s yet", ty_str(g->tys, t));
}

static Class classify(Gen *g, const Ty *t, int line, int col) {
    switch (t->kind) {
    case TY_VOID:
    case TY_NEVER:
        return C_VOID;
    case TY_BOOL:
    case TY_INT:
    case TY_PTR:
        return C_SCALAR;
    case TY_STRUCT:
    case TY_ARRAY:
    case TY_SLICE:
        return C_AGG;
    default:
        unsupported(g, line, col, t);
    }
}

static void layout(Gen *g, Ty *t, size_t *size, size_t *align, int line, int col);

static void struct_layout(Gen *g, StructInfo *si, int line, int col) {
    size_t cur = 0, maxalign = 1, i;

    if (si->laid_out) {
        return;
    }
    si->offsets = arena_alloc(g->arena, (si->nfields + 1) * sizeof(size_t));
    for (i = 0; i < si->nfields; i++) {
        size_t fs, fa;
        layout(g, si->fields[i].ty, &fs, &fa, line, col);
        cur = align_up(cur, fa);
        si->offsets[i] = cur;
        cur += fs;
        if (fa > maxalign) {
            maxalign = fa;
        }
    }
    si->align = maxalign;
    si->size = align_up(cur, maxalign);
    si->laid_out = 1;
}

static void layout(Gen *g, Ty *t, size_t *size, size_t *align, int line, int col) {
    switch (t->kind) {
    case TY_VOID:
    case TY_NEVER:
        *size = 0;
        *align = 1;
        return;
    case TY_BOOL:
        *size = 1;
        *align = 1;
        return;
    case TY_INT:
        *size = (size_t)t->bits / 8;
        *align = *size;
        return;
    case TY_PTR:
        *size = 8;
        *align = 8;
        return;
    case TY_SLICE:
        *size = 16;
        *align = 8;
        return;
    case TY_ARRAY: {
        size_t es, ea;
        layout(g, t->elem, &es, &ea, line, col);
        *size = es * t->len;
        *align = ea;
        return;
    }
    case TY_STRUCT:
        struct_layout(g, t->strukt, line, col);
        *size = t->strukt->size;
        *align = t->strukt->align;
        return;
    default:
        unsupported(g, line, col, t);
    }
}

static size_t size_of(Gen *g, Ty *t, int line, int col) {
    size_t s, a;
    layout(g, t, &s, &a, line, col);
    return s;
}

/* Index of the field called `name`, and the layout of its struct. */
static size_t field_index(Gen *g, StructInfo *si, Str name, int line, int col) {
    size_t i;

    struct_layout(g, si, line, col);
    for (i = 0; i < si->nfields; i++) {
        if (si->fields[i].name.len == name.len && memcmp(si->fields[i].name.ptr, name.ptr, name.len) == 0) {
            return i;
        }
    }
    fail(g, line, col, "internal error: no field '%.*s'", (int)name.len, name.ptr);
}

/* ---- frame ---- */

/* Reserves `size` bytes in the frame; returns the offset from rbp (negative). */
static long alloc_slot(Gen *g, size_t size, size_t align) {
    g->frame = (long)align_up((size_t)g->frame + (size ? size : 1), align < 8 ? 8 : align);
    return -g->frame;
}

/* ---- loads, stores, conversions ---- */

static int is_signed(const Ty *t) {
    return t->kind == TY_INT && t->is_signed;
}

static int scalar_size(const Ty *t) {
    if (t->kind == TY_BOOL) {
        return 1;
    }
    if (t->kind == TY_INT) {
        return t->bits / 8;
    }
    return 8; /* pointers */
}

/* dst = the scalar of type t stored at mem, in canonical form (sign- or zero-extended) */
static void load_scalar(Gen *g, const Ty *t, Opd mem, Reg dst) {
    int sz = scalar_size(t);

    switch (sz) {
    case 1:
    case 2:
        asm_op2(g->as, is_signed(t) ? X_MOVSX : X_MOVZX, sz, REG(dst), mem);
        break;
    case 4:
        if (is_signed(t)) {
            asm_op2(g->as, X_MOVSXD, 4, REG(dst), mem);
        } else {
            asm_op2(g->as, X_MOV, 4, REG(dst), mem);
        }
        break;
    default:
        asm_op2(g->as, X_MOV, 8, REG(dst), mem);
        break;
    }
}

static void store_scalar(Gen *g, const Ty *t, Opd mem, Reg src) {
    asm_op2(g->as, X_MOV, scalar_size(t), mem, REG(src));
}

/* Brings rax back to the canonical form of type t after an operation that may have
 * left garbage in the upper bits (wrap-around arithmetic). */
static void normalize(Gen *g, const Ty *t) {
    int sz = scalar_size(t);

    if (t->kind != TY_INT) {
        return;
    }
    switch (sz) {
    case 1:
    case 2:
        asm_op2(g->as, is_signed(t) ? X_MOVSX : X_MOVZX, sz, REG(RAX), REG(RAX));
        break;
    case 4:
        if (is_signed(t)) {
            asm_op2(g->as, X_MOVSXD, 4, REG(RAX), REG(RAX));
        } else {
            asm_op2(g->as, X_MOV, 4, REG(RAX), REG(RAX));
        }
        break;
    default:
        break;
    }
}

/* Copies `size` bytes from the address in rsi to the address in rdi. */
static void copy_bytes(Gen *g, size_t size) {
    asm_op2(g->as, X_MOV, 8, REG(RCX), IMM((long long)size));
    asm_op0(g->as, X_REP_MOVSB);
}

/* ---- functions and labels ---- */

static const char *qualified_name(Gen *g, const Sym *sy) {
    size_t len = sy->name.len + 1;
    const StructInfo *o;
    char *out, *w;

    for (o = sy->owner; o; o = o->parent) {
        if (o->name.len) {
            len += o->name.len + 1;
        }
    }
    out = arena_alloc(g->arena, len);
    w = out + len - 1;
    *w = '\0';
    w -= sy->name.len;
    memcpy(w, sy->name.ptr, sy->name.len);
    for (o = sy->owner; o; o = o->parent) {
        if (o->name.len) {
            *--w = '.';
            w -= o->name.len;
            memcpy(w, o->name.ptr, o->name.len);
        }
    }
    return out;
}

/* The label of a function, queueing it for generation the first time it is used. */
static int request_func(Gen *g, Sym *fn) {
    if (!fn->has_label) {
        fn->label = asm_new_label(g->as, qualified_name(g, fn));
        fn->has_label = 1;
        vec_push(g->arena, &g->worklist, &fn);
    }
    return fn->label;
}

/* ---- expressions ---- */

static void gen_expr(Gen *g, Expr *e);
static void gen_place(Gen *g, Expr *e);
static void load_through_rax(Gen *g, Expr *e);

static long long literal_value(Gen *g, const Expr *e) {
    unsigned long long v;
    if (!parse_int_literal(e->u.lit, &v)) {
        fail(g, e->line, e->col, "the integer literal is too large");
    }
    return (long long)v; /* the bit pattern; canonical for the literal's type */
}

static Cond cond_for(TokenKind op, int sgn) {
    switch (op) {
    case TOK_EQ: return CC_E;
    case TOK_NE: return CC_NE;
    case TOK_LT: return sgn ? CC_L : CC_B;
    case TOK_LE: return sgn ? CC_LE : CC_BE;
    case TOK_GT: return sgn ? CC_G : CC_A;
    default: return sgn ? CC_GE : CC_AE; /* TOK_GE */
    }
}

static Cond negate_cond(Cond c) {
    switch (c) {
    case CC_E: return CC_NE;
    case CC_NE: return CC_E;
    case CC_L: return CC_GE;
    case CC_GE: return CC_L;
    case CC_LE: return CC_G;
    case CC_G: return CC_LE;
    case CC_B: return CC_AE;
    case CC_AE: return CC_B;
    case CC_BE: return CC_A;
    default: return CC_BE; /* CC_A */
    }
}

static int is_comparison(TokenKind op) {
    return op == TOK_EQ || op == TOK_NE || op == TOK_LT || op == TOK_LE || op == TOK_GT || op == TOK_GE;
}

/* An operand that can be loaded straight into rcx without disturbing rax. */
static int simple_operand(const Expr *e) {
    if (e->kind == EXPR_INT || e->kind == EXPR_BOOL) {
        return 1;
    }
    return e->kind == EXPR_IDENT && e->sym && e->sym->kind == SYM_LOCAL && e->ty->kind != TY_STRUCT &&
           e->ty->kind != TY_ARRAY && e->ty->kind != TY_SLICE;
}

static void gen_simple_into(Gen *g, Expr *e, Reg dst) {
    if (e->kind == EXPR_INT) {
        asm_op2(g->as, X_MOV, 8, REG(dst), IMM(literal_value(g, e)));
    } else if (e->kind == EXPR_BOOL) {
        asm_op2(g->as, X_MOV, 8, REG(dst), IMM(e->u.boolean ? 1 : 0));
    } else {
        load_scalar(g, e->ty, MEM(RBP, e->sym->frame_off), dst);
    }
}

/* lhs in rax, rhs in rcx */
static void gen_operands(Gen *g, Expr *l, Expr *r) {
    if (simple_operand(r)) {
        gen_expr(g, l);
        gen_simple_into(g, r, RCX);
        return;
    }
    gen_expr(g, l);
    asm_op1(g->as, X_PUSH, REG(RAX));
    gen_expr(g, r);
    asm_op2(g->as, X_MOV, 8, REG(RCX), REG(RAX));
    asm_op1(g->as, X_POP, REG(RAX));
}

/* Jumps to `label` when the boolean expression e equals `when`. */
static void gen_branch(Gen *g, Expr *e, int label, int when) {
    if (e->kind == EXPR_BOOL) {
        if ((e->u.boolean != 0) == (when != 0)) {
            asm_op1(g->as, X_JMP, opd_label(label));
        }
        return;
    }
    if (e->kind == EXPR_UNARY && e->u.unary.op == TOK_BANG) {
        gen_branch(g, e->u.unary.operand, label, !when);
        return;
    }
    if (e->kind == EXPR_BINARY && is_comparison(e->u.binary.op)) {
        Cond cc = cond_for(e->u.binary.op, is_signed(e->u.binary.lhs->ty));
        gen_operands(g, e->u.binary.lhs, e->u.binary.rhs);
        asm_op2(g->as, X_CMP, 8, REG(RAX), REG(RCX));
        asm_jcc(g->as, when ? cc : negate_cond(cc), label);
        return;
    }
    if (e->kind == EXPR_BINARY && e->u.binary.op == TOK_ANDAND) {
        if (when) { /* jump if both are true */
            int skip = asm_new_label(g->as, NULL);
            gen_branch(g, e->u.binary.lhs, skip, 0);
            gen_branch(g, e->u.binary.rhs, label, 1);
            asm_label(g->as, skip);
        } else { /* jump if either is false */
            gen_branch(g, e->u.binary.lhs, label, 0);
            gen_branch(g, e->u.binary.rhs, label, 0);
        }
        return;
    }
    if (e->kind == EXPR_BINARY && e->u.binary.op == TOK_OROR) {
        if (when) { /* jump if either is true */
            gen_branch(g, e->u.binary.lhs, label, 1);
            gen_branch(g, e->u.binary.rhs, label, 1);
        } else { /* jump if both are false */
            int skip = asm_new_label(g->as, NULL);
            gen_branch(g, e->u.binary.lhs, skip, 1);
            gen_branch(g, e->u.binary.rhs, label, 0);
            asm_label(g->as, skip);
        }
        return;
    }
    gen_expr(g, e);
    asm_op2(g->as, X_TEST, 8, REG(RAX), REG(RAX));
    asm_jcc(g->as, when ? CC_NE : CC_E, label);
}

static void gen_binary(Gen *g, Expr *e) {
    TokenKind op = e->u.binary.op;
    Expr *l = e->u.binary.lhs, *r = e->u.binary.rhs;
    Ty *t = e->ty;

    if (op == TOK_ANDAND || op == TOK_OROR) {
        int end = asm_new_label(g->as, NULL);
        gen_expr(g, l);
        asm_op2(g->as, X_TEST, 8, REG(RAX), REG(RAX));
        asm_jcc(g->as, op == TOK_ANDAND ? CC_E : CC_NE, end); /* rax already holds the answer */
        gen_expr(g, r);
        asm_label(g->as, end);
        return;
    }
    if (is_comparison(op)) {
        gen_operands(g, l, r);
        asm_op2(g->as, X_CMP, 8, REG(RAX), REG(RCX));
        asm_setcc(g->as, cond_for(op, is_signed(l->ty)), RAX);
        asm_op2(g->as, X_MOVZX, 1, REG(RAX), REG(RAX));
        return;
    }
    gen_operands(g, l, r);
    switch (op) {
    case TOK_PLUS:
        asm_op2(g->as, X_ADD, 8, REG(RAX), REG(RCX));
        break;
    case TOK_MINUS:
        asm_op2(g->as, X_SUB, 8, REG(RAX), REG(RCX));
        break;
    case TOK_STAR:
        asm_op2(g->as, X_IMUL, 8, REG(RAX), REG(RCX));
        break;
    case TOK_SLASH:
    case TOK_PERCENT:
        if (is_signed(t)) {
            asm_op0(g->as, X_CQO);
            asm_op1(g->as, X_IDIV, REG(RCX));
        } else {
            asm_op2(g->as, X_MOV, 8, REG(RDX), IMM(0));
            asm_op1(g->as, X_DIV, REG(RCX));
        }
        if (op == TOK_PERCENT) {
            asm_op2(g->as, X_MOV, 8, REG(RAX), REG(RDX));
        }
        break;
    case TOK_AMP:
        asm_op2(g->as, X_AND, 8, REG(RAX), REG(RCX));
        break;
    case TOK_PIPE:
        asm_op2(g->as, X_OR, 8, REG(RAX), REG(RCX));
        break;
    case TOK_CARET:
        asm_op2(g->as, X_XOR, 8, REG(RAX), REG(RCX));
        break;
    case TOK_SHL:
        asm_op2(g->as, X_SHL, 8, REG(RAX), REG(RCX));
        break;
    case TOK_SHR:
        asm_op2(g->as, is_signed(t) ? X_SAR : X_SHR, 8, REG(RAX), REG(RCX));
        break;
    default:
        fail(g, e->line, e->col, "unsupported operator");
    }
    normalize(g, t);
}

static void gen_unary(Gen *g, Expr *e) {
    Expr *x = e->u.unary.operand;

    switch (e->u.unary.op) {
    case TOK_MINUS:
        gen_expr(g, x);
        asm_op1(g->as, X_NEG, REG(RAX));
        normalize(g, e->ty);
        break;
    case TOK_BANG:
        gen_expr(g, x);
        asm_op2(g->as, X_XOR, 8, REG(RAX), IMM(1));
        break;
    case TOK_TILDE:
        gen_expr(g, x);
        asm_op1(g->as, X_NOT, REG(RAX));
        normalize(g, e->ty);
        break;
    case TOK_STAR: /* *p: the pointer is the address */
        gen_expr(g, x);
        load_through_rax(g, e);
        break;
    case TOK_AMP:
        gen_place(g, x);
        break;
    default:
        fail(g, e->line, e->col, "this operator is not supported by the code generator yet");
    }
}

/* Integer and pointer conversions: the operand is in canonical form for its own type,
 * so converting means re-normalizing for the target type. */
static void gen_cast(Gen *g, Expr *e) {
    gen_expr(g, e->u.cast.operand);
    normalize(g, e->ty);
}

/* Pushes the call's arguments (right to left), calls, pops them. The result is in rax. */
static void gen_call(Gen *g, Expr *e) {
    Sym *fn = e->sym;
    Ty *ft = fn->ty;
    int method = e->u.call.is_method;
    size_t nargs = e->u.call.nargs + (method ? 1 : 0);
    size_t i;
    long pushed = 0;
    int label = request_func(g, fn);
    Class rc = classify(g, ft->ret, e->line, e->col);
    long tmp = 0;

    for (i = nargs; i-- > 0;) {
        Ty *pt = ft->params[i];
        Class pc = classify(g, pt, e->line, e->col);

        if (method && i == 0) {
            Expr *base = e->u.call.callee->u.field.base;
            if (e->u.call.recv_adjust == 1) {
                gen_place(g, base); /* pass its address */
            } else {
                gen_expr(g, base);
            }
        } else {
            gen_expr(g, e->u.call.args[method ? i - 1 : i]);
        }

        if (pc == C_AGG) {
            size_t sz = size_of(g, pt, e->line, e->col);
            size_t slot = align_up(sz, 8);
            asm_op2(g->as, X_MOV, 8, REG(RSI), REG(RAX));
            if (slot) {
                asm_op2(g->as, X_SUB, 8, REG(RSP), IMM((long long)slot));
            }
            asm_op2(g->as, X_MOV, 8, REG(RDI), REG(RSP));
            copy_bytes(g, sz);
            pushed += (long)slot;
        } else {
            asm_op1(g->as, X_PUSH, REG(RAX));
            pushed += 8;
        }
    }
    if (rc == C_AGG) {
        size_t sz, al;
        layout(g, ft->ret, &sz, &al, e->line, e->col);
        tmp = alloc_slot(g, sz, al);
        asm_op2(g->as, X_LEA, 8, REG(RAX), MEM(RBP, tmp));
        asm_op1(g->as, X_PUSH, REG(RAX)); /* the hidden result pointer is the first argument */
        pushed += 8;
    }
    asm_op1(g->as, X_CALL, opd_label(label));
    if (pushed) {
        asm_op2(g->as, X_ADD, 8, REG(RSP), IMM(pushed));
    }
}

static void emit_defers_from(Gen *g, size_t mark) {
    size_t i;
    Expr **d = g->defers.data;

    if (g->defers.len == mark) {
        return;
    }
    asm_op1(g->as, X_PUSH, REG(RAX)); /* keep the block's value */
    g->in_defer++;
    for (i = g->defers.len; i-- > mark;) {
        gen_expr(g, d[i]);
    }
    g->in_defer--;
    asm_op1(g->as, X_POP, REG(RAX));
}

static void gen_stmt(Gen *g, Stmt *st);

static void gen_block(Gen *g, Expr *e) {
    Block *b = e->u.block;
    size_t mark = g->defers.len, i;

    for (i = 0; i < b->nstmts; i++) {
        gen_stmt(g, b->stmts[i]);
    }
    if (b->tail) {
        gen_expr(g, b->tail);
    }
    emit_defers_from(g, mark);
    g->defers.len = mark;
}

static void gen_if(Gen *g, Expr *e) {
    int else_l = asm_new_label(g->as, NULL);

    gen_branch(g, e->u.if_.cond, else_l, 0);
    gen_expr(g, e->u.if_.then_block);
    if (e->u.if_.else_expr) {
        int end_l = asm_new_label(g->as, NULL);
        asm_op1(g->as, X_JMP, opd_label(end_l));
        asm_label(g->as, else_l);
        gen_expr(g, e->u.if_.else_expr);
        asm_label(g->as, end_l);
    } else {
        asm_label(g->as, else_l);
    }
}

static void gen_while(Gen *g, Expr *e) {
    int top = asm_new_label(g->as, NULL);
    int end = asm_new_label(g->as, NULL);

    asm_label(g->as, top);
    gen_branch(g, e->u.while_.cond, end, 0);
    gen_expr(g, e->u.while_.body);
    asm_op1(g->as, X_JMP, opd_label(top));
    asm_label(g->as, end);
}

/* The value of the right-hand side is stored into the place of the left-hand side. */
static void gen_assign(Gen *g, Expr *e) {
    Expr *l = e->u.binary.lhs, *r = e->u.binary.rhs;
    Class c = classify(g, l->ty, e->line, e->col);

    if (c == C_SCALAR) {
        gen_expr(g, r);
        if (l->kind == EXPR_IDENT) {
            store_scalar(g, l->ty, MEM(RBP, l->sym->frame_off), RAX);
        } else {
            asm_op1(g->as, X_PUSH, REG(RAX));
            gen_place(g, l);
            asm_op1(g->as, X_POP, REG(RCX));
            store_scalar(g, l->ty, MEM(RAX, 0), RCX);
        }
        return;
    }
    gen_expr(g, r); /* the address of the source */
    asm_op1(g->as, X_PUSH, REG(RAX));
    gen_place(g, l);
    asm_op2(g->as, X_MOV, 8, REG(RDI), REG(RAX));
    asm_op1(g->as, X_POP, REG(RSI));
    copy_bytes(g, size_of(g, l->ty, e->line, e->col));
}

/* Builds the struct in a fresh frame slot, field by field. rax = its address. */
static void gen_struct_lit(Gen *g, Expr *e) {
    StructInfo *si = e->ty->strukt;
    size_t sz, al, i;
    long slot;

    layout(g, e->ty, &sz, &al, e->line, e->col);
    slot = alloc_slot(g, sz, al);
    for (i = 0; i < e->u.struct_lit.nfields; i++) {
        FieldInit *fi = e->u.struct_lit.fields[i];
        size_t j = field_index(g, si, fi->name, fi->line, fi->col);
        Ty *ft = si->fields[j].ty;
        long at = slot + (long)si->offsets[j];

        gen_expr(g, fi->value);
        if (classify(g, ft, fi->line, fi->col) == C_AGG) {
            asm_op2(g->as, X_MOV, 8, REG(RSI), REG(RAX));
            asm_op2(g->as, X_LEA, 8, REG(RDI), MEM(RBP, at));
            copy_bytes(g, size_of(g, ft, fi->line, fi->col));
        } else {
            store_scalar(g, ft, MEM(RBP, at), RAX);
        }
    }
    asm_op2(g->as, X_LEA, 8, REG(RAX), MEM(RBP, slot));
}

/* rax = the scalar of type e->ty at the address in rax, or (for structs) just the address */
static void load_through_rax(Gen *g, Expr *e) {
    if (classify(g, e->ty, e->line, e->col) == C_SCALAR) {
        load_scalar(g, e->ty, MEM(RAX, 0), RAX);
    }
}

static void gen_expr(Gen *g, Expr *e) {
    switch (e->kind) {
    case EXPR_INT:
        asm_op2(g->as, X_MOV, 8, REG(RAX), IMM(literal_value(g, e)));
        break;
    case EXPR_BOOL:
        asm_op2(g->as, X_MOV, 8, REG(RAX), IMM(e->u.boolean ? 1 : 0));
        break;
    case EXPR_IDENT:
        if (classify(g, e->ty, e->line, e->col) == C_AGG) {
            asm_op2(g->as, X_LEA, 8, REG(RAX), MEM(RBP, e->sym->frame_off));
        } else {
            load_scalar(g, e->ty, MEM(RBP, e->sym->frame_off), RAX);
        }
        break;
    case EXPR_UNARY:
        gen_unary(g, e);
        break;
    case EXPR_BINARY:
        gen_binary(g, e);
        break;
    case EXPR_CAST:
        gen_cast(g, e);
        break;
    case EXPR_CALL:
        gen_call(g, e);
        break;
    case EXPR_BLOCK:
        gen_block(g, e);
        break;
    case EXPR_IF:
        gen_if(g, e);
        break;
    case EXPR_WHILE:
        gen_while(g, e);
        break;
    case EXPR_ASSIGN:
        gen_assign(g, e);
        break;
    case EXPR_FIELD:
    case EXPR_INDEX:
        gen_place(g, e);
        load_through_rax(g, e);
        break;
    case EXPR_STRUCT_LIT:
        gen_struct_lit(g, e);
        break;
    default:
        fail(g, e->line, e->col, "this kind of expression is not supported by the code generator yet");
    }
}

/* rax = the address of the place denoted by e */
static void gen_place(Gen *g, Expr *e) {
    switch (e->kind) {
    case EXPR_IDENT:
        asm_op2(g->as, X_LEA, 8, REG(RAX), MEM(RBP, e->sym->frame_off));
        break;
    case EXPR_FIELD: {
        Expr *base = e->u.field.base;
        Ty *st = base->ty->kind == TY_PTR ? base->ty->elem : base->ty;
        size_t j = field_index(g, st->strukt, e->u.field.name, e->line, e->col);
        size_t off = st->strukt->offsets[j];

        gen_expr(g, base); /* a pointer to the struct, or the address of the struct value */
        if (off) {
            asm_op2(g->as, X_ADD, 8, REG(RAX), IMM((long long)off));
        }
        break;
    }
    case EXPR_INDEX: {
        Expr *base = e->u.index.base;
        size_t es;

        if (base->ty->kind != TY_PTR) {
            fail(g, e->line, e->col, "indexing %s is not supported by the code generator yet (pointers are)",
                 ty_str(g->tys, base->ty));
        }
        es = size_of(g, base->ty->elem, e->line, e->col);
        gen_expr(g, base);
        asm_op1(g->as, X_PUSH, REG(RAX));
        gen_expr(g, e->u.index.index);
        if (es != 1) {
            asm_op2(g->as, X_MOV, 8, REG(RCX), IMM((long long)es));
            asm_op2(g->as, X_IMUL, 8, REG(RAX), REG(RCX));
        }
        asm_op1(g->as, X_POP, REG(RCX));
        asm_op2(g->as, X_ADD, 8, REG(RAX), REG(RCX));
        break;
    }
    case EXPR_UNARY:
        if (e->u.unary.op == TOK_STAR) {
            gen_expr(g, e->u.unary.operand); /* the pointer is the address */
            break;
        }
        fail(g, e->line, e->col, "this kind of place is not supported by the code generator yet");
    default:
        fail(g, e->line, e->col, "this kind of place is not supported by the code generator yet");
    }
}

/* ---- statements ---- */

/* Moves the value of the function body (or of a `return`) from rax to where the
 * caller expects it: scalars stay in rax; a struct is copied to the hidden pointer. */
static void emit_result(Gen *g, Ty *value_ty, int line, int col) {
    if (value_ty->kind == TY_NEVER || classify(g, g->ret, line, col) != C_AGG) {
        return;
    }
    asm_op2(g->as, X_MOV, 8, REG(RSI), REG(RAX));
    asm_op2(g->as, X_MOV, 8, REG(RDI), MEM(RBP, g->retptr_off));
    copy_bytes(g, size_of(g, g->ret, line, col));
    asm_op2(g->as, X_MOV, 8, REG(RAX), MEM(RBP, g->retptr_off));
}

static void gen_stmt(Gen *g, Stmt *st) {
    switch (st->kind) {
    case STMT_LET:
    case STMT_VAR: {
        Ty *t = st->sym->ty;
        size_t sz, al;
        long off;

        layout(g, t, &sz, &al, st->line, st->col);
        gen_expr(g, st->expr);
        off = alloc_slot(g, sz, al);
        st->sym->frame_off = off;
        if (classify(g, t, st->line, st->col) == C_AGG) {
            asm_op2(g->as, X_MOV, 8, REG(RSI), REG(RAX));
            asm_op2(g->as, X_LEA, 8, REG(RDI), MEM(RBP, off));
            copy_bytes(g, sz);
        } else {
            store_scalar(g, t, MEM(RBP, off), RAX);
        }
        break;
    }
    case STMT_DEFER:
        vec_push(g->arena, &g->defers, &st->expr);
        break;
    case STMT_RETURN:
        if (g->in_defer) {
            fail(g, st->line, st->col, "'return' is not allowed inside a deferred expression");
        }
        if (st->expr) {
            gen_expr(g, st->expr);
            emit_result(g, st->expr->ty, st->line, st->col);
        }
        emit_defers_from(g, 0);
        asm_op1(g->as, X_JMP, opd_label(g->ret_label));
        break;
    case STMT_EXPR:
        gen_expr(g, st->expr);
        break;
    }
}

/* ---- functions ---- */

static void gen_func(Gen *g, Sym *fn) {
    Item *it = fn->item;
    Ty *ft = fn->ty;
    size_t sub_idx, i;
    long off = 16;

    g->cur_file = it->file;
    g->fn = fn;
    g->ret = ft->ret;
    g->frame = 0;
    g->defers.len = 0;
    g->in_defer = 0;
    g->ret_label = asm_new_label(g->as, NULL);

    asm_label(g->as, fn->label);
    asm_op1(g->as, X_PUSH, REG(RBP));
    asm_op2(g->as, X_MOV, 8, REG(RBP), REG(RSP));
    sub_idx = asm_count(g->as);
    asm_op2(g->as, X_SUB, 8, REG(RSP), IMM(0)); /* the frame size is patched in below */

    /* where the caller put the arguments */
    if (classify(g, ft->ret, it->line, it->col) == C_AGG) {
        g->retptr_off = off;
        off += 8;
    }
    for (i = 0; i < it->u.func.nparams; i++) {
        Param *p = it->u.func.params[i];
        p->sym->frame_off = off;
        if (classify(g, ft->params[i], p->line, p->col) == C_AGG) {
            off += (long)align_up(size_of(g, ft->params[i], p->line, p->col), 8);
        } else {
            off += 8; /* a scalar takes one pushed slot */
        }
    }

    gen_expr(g, it->u.func.body);
    emit_result(g, it->u.func.body->ty, it->line, it->col);
    asm_label(g->as, g->ret_label);
    asm_op2(g->as, X_MOV, 8, REG(RSP), REG(RBP));
    asm_op1(g->as, X_POP, REG(RBP));
    asm_op0(g->as, X_RET);

    if (g->frame == 0) {
        asm_remove(g->as, sub_idx); /* no locals: no need to reserve a frame */
    } else {
        asm_set_imm(g->as, sub_idx, (long long)align_up((size_t)g->frame, 8));
    }
}

static Sym *find_main(Gen *g) {
    StructInfo *root = sema_root(g->sema);
    size_t i;

    for (i = 0; i < root->nmembers; i++) {
        Sym *m = root->members[i];
        if (m->kind == SYM_FUNC && m->name.len == 4 && memcmp(m->name.ptr, "main", 4) == 0) {
            return m;
        }
    }
    return NULL;
}

int codegen(Sema *sema, Asm *out, int *entry_label, CodegenError *err) {
    Gen g;
    Sym *main_fn;
    Ty *mret;
    int start;
    size_t i;

    memset(&g, 0, sizeof g);
    g.sema = sema;
    g.tys = sema_types(sema);
    g.as = out;
    g.arena = out->arena;
    g.err = err;
    err->file[0] = '\0';
    err->msg[0] = '\0';
    err->line = 0;
    err->col = 0;
    vec_init(&g.worklist, sizeof(Sym *));
    vec_init(&g.defers, sizeof(Expr *));
    if (setjmp(g.jb)) {
        return 0;
    }

    main_fn = find_main(&g);
    if (!main_fn) {
        fail(&g, 1, 1, "there is no 'main' function");
    }
    g.cur_file = main_fn->item->file;
    mret = main_fn->ty->ret;
    if (main_fn->ty->nparams != 0) {
        fail(&g, main_fn->item->line, main_fn->item->col, "'main' cannot take parameters");
    }
    if (mret->kind != TY_VOID && mret->kind != TY_INT && mret->kind != TY_BOOL) {
        fail(&g, main_fn->item->line, main_fn->item->col,
             "'main' must return void or an integer, not %s", ty_str(g.tys, mret));
    }

    /* _start: call main, exit with its result */
    start = asm_new_label(g.as, "_start");
    asm_label(g.as, start);
    asm_op1(g.as, X_CALL, opd_label(request_func(&g, main_fn)));
    if (mret->kind == TY_VOID) {
        asm_op2(g.as, X_MOV, 8, REG(RDI), IMM(0));
    } else {
        asm_op2(g.as, X_MOV, 8, REG(RDI), REG(RAX));
    }
    asm_op2(g.as, X_MOV, 8, REG(RAX), IMM(60)); /* exit */
    asm_op0(g.as, X_SYSCALL);

    for (i = 0; i < g.worklist.len; i++) {
        gen_func(&g, ((Sym **)g.worklist.data)[i]);
    }
    *entry_label = start;
    return 1;
}
