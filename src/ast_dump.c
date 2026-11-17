#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ast.h"

/* Compact S-expression dump of the tree, used for debugging and in tests. */

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} Buf;

static void buf_put(Buf *b, const char *s, size_t n) {
    if (b->len + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 256;
        char *nd;
        while (b->len + n + 1 > cap) {
            cap *= 2;
        }
        nd = realloc(b->data, cap);
        if (!nd) {
            fprintf(stderr, "gale: out of memory\n");
            exit(1);
        }
        b->data = nd;
        b->cap = cap;
    }
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = '\0';
}

static void put(Buf *b, const char *s) {
    buf_put(b, s, strlen(s));
}

static void put_str(Buf *b, Str s) {
    buf_put(b, s.ptr, s.len);
}

static void put_path(Buf *b, Path p) {
    size_t i;
    for (i = 0; i < p.count; i++) {
        if (i) {
            put(b, ".");
        }
        put_str(b, p.segs[i]);
    }
}

static void dump_expr(Buf *b, const Expr *e);

static const char *op_text(TokenKind k) {
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
    case TOK_ASSIGN: return "=";
    case TOK_KW_TRY: return "try";
    default: return "?";
    }
}

static void dump_type(Buf *b, const Type *t) {
    switch (t->kind) {
    case TYPE_PATH:
        put_path(b, t->u.path);
        break;
    case TYPE_PTR:
        put(b, "(ptr ");
        dump_type(b, t->u.elem);
        put(b, ")");
        break;
    case TYPE_OPTIONAL:
        put(b, "(opt ");
        dump_type(b, t->u.elem);
        put(b, ")");
        break;
    case TYPE_SLICE:
        put(b, "(slice ");
        dump_type(b, t->u.elem);
        put(b, ")");
        break;
    case TYPE_ARRAY:
        put(b, "(array ");
        dump_expr(b, t->u.array.len);
        put(b, " ");
        dump_type(b, t->u.array.elem);
        put(b, ")");
        break;
    case TYPE_ERRUNION:
        put(b, "(err ");
        dump_type(b, t->u.elem);
        put(b, ")");
        break;
    }
}

static void dump_expr(Buf *b, const Expr *e) {
    switch (e->kind) {
    case EXPR_INT:
    case EXPR_FLOAT:
    case EXPR_STRING:
    case EXPR_IDENT:
        put_str(b, e->u.lit);
        break;
    case EXPR_BOOL:
        put(b, e->u.boolean ? "true" : "false");
        break;
    case EXPR_UNARY: {
        const char *op = op_text(e->u.unary.op);
        if (e->u.unary.op == TOK_STAR) {
            op = "deref";
        } else if (e->u.unary.op == TOK_AMP) {
            op = "addr";
        }
        put(b, "(");
        put(b, op);
        put(b, " ");
        dump_expr(b, e->u.unary.operand);
        put(b, ")");
        break;
    }
    case EXPR_BINARY:
    case EXPR_ASSIGN:
        put(b, "(");
        put(b, op_text(e->u.binary.op));
        put(b, " ");
        dump_expr(b, e->u.binary.lhs);
        put(b, " ");
        dump_expr(b, e->u.binary.rhs);
        put(b, ")");
        break;
    case EXPR_CAST:
        put(b, "(as ");
        dump_expr(b, e->u.cast.operand);
        put(b, " ");
        dump_type(b, e->u.cast.type);
        put(b, ")");
        break;
    case EXPR_BLOCK: {
        const Block *blk = e->u.block;
        size_t i;
        put(b, "(block");
        for (i = 0; i < blk->nstmts; i++) {
            const Stmt *st = blk->stmts[i];
            put(b, " ");
            switch (st->kind) {
            case STMT_LET:
            case STMT_VAR:
                put(b, st->kind == STMT_LET ? "(let " : "(var ");
                put_str(b, st->name);
                put(b, " ");
                if (st->type) {
                    dump_type(b, st->type);
                    put(b, " ");
                }
                dump_expr(b, st->expr);
                put(b, ")");
                break;
            case STMT_DEFER:
                put(b, "(defer ");
                dump_expr(b, st->expr);
                put(b, ")");
                break;
            case STMT_RETURN:
                put(b, "(return");
                if (st->expr) {
                    put(b, " ");
                    dump_expr(b, st->expr);
                }
                put(b, ")");
                break;
            case STMT_EXPR:
                put(b, "(expr ");
                dump_expr(b, st->expr);
                put(b, ")");
                break;
            }
        }
        if (blk->tail) {
            put(b, " (tail ");
            dump_expr(b, blk->tail);
            put(b, ")");
        }
        put(b, ")");
        break;
    }
    case EXPR_IF:
        put(b, "(if ");
        dump_expr(b, e->u.if_.cond);
        put(b, " ");
        dump_expr(b, e->u.if_.then_block);
        if (e->u.if_.else_expr) {
            put(b, " ");
            dump_expr(b, e->u.if_.else_expr);
        }
        put(b, ")");
        break;
    case EXPR_WHILE:
        put(b, "(while ");
        dump_expr(b, e->u.while_.cond);
        put(b, " ");
        dump_expr(b, e->u.while_.body);
        put(b, ")");
        break;
    case EXPR_STRUCT_LIT: {
        size_t i;
        put(b, "(lit ");
        put_path(b, e->u.struct_lit.path);
        for (i = 0; i < e->u.struct_lit.nfields; i++) {
            put(b, " (");
            put_str(b, e->u.struct_lit.fields[i]->name);
            put(b, " ");
            dump_expr(b, e->u.struct_lit.fields[i]->value);
            put(b, ")");
        }
        put(b, ")");
        break;
    }
    case EXPR_CALL: {
        size_t i;
        put(b, "(call ");
        dump_expr(b, e->u.call.callee);
        for (i = 0; i < e->u.call.nargs; i++) {
            put(b, " ");
            dump_expr(b, e->u.call.args[i]);
        }
        put(b, ")");
        break;
    }
    case EXPR_INDEX:
        put(b, "(index ");
        dump_expr(b, e->u.index.base);
        put(b, " ");
        dump_expr(b, e->u.index.index);
        put(b, ")");
        break;
    case EXPR_FIELD:
        put(b, "(. ");
        dump_expr(b, e->u.field.base);
        put(b, " ");
        put_str(b, e->u.field.name);
        put(b, ")");
        break;
    default:
        put(b, "?");
        break;
    }
}

static void dump_item(Buf *b, const Item *it);

static void dump_body(Buf *b, const Body *body) {
    size_t i;
    for (i = 0; i < body->nfields; i++) {
        put(b, " (");
        put_str(b, body->fields[i]->name);
        put(b, " ");
        dump_type(b, body->fields[i]->type);
        put(b, ")");
    }
    for (i = 0; i < body->nitems; i++) {
        put(b, " ");
        dump_item(b, body->items[i]);
    }
}

static void dump_item(Buf *b, const Item *it) {
    put(b, "(");
    if (it->exported) {
        put(b, "export ");
    }
    switch (it->kind) {
    case ITEM_UNFOLD:
        put(b, "unfold ");
        put_path(b, it->u.unfold);
        break;
    case ITEM_STRUCT:
        put(b, "struct ");
        put_str(b, it->u.struct_.name);
        dump_body(b, &it->u.struct_.body);
        break;
    case ITEM_FUNC: {
        size_t i;
        put(b, "func ");
        put_str(b, it->u.func.name);
        put(b, " (");
        for (i = 0; i < it->u.func.nparams; i++) {
            if (i) {
                put(b, " ");
            }
            put(b, "(");
            put_str(b, it->u.func.params[i]->name);
            put(b, " ");
            dump_type(b, it->u.func.params[i]->type);
            put(b, ")");
        }
        put(b, ") ");
        if (it->u.func.ret) {
            dump_type(b, it->u.func.ret);
        } else {
            put(b, "void");
        }
        put(b, " ");
        dump_expr(b, it->u.func.body);
        break;
    }
    }
    put(b, ")");
}

char *ast_dump(const File *file) {
    Buf b;
    size_t i;

    b.data = NULL;
    b.len = 0;
    b.cap = 0;
    put(&b, ""); /* make sure the buffer exists even for an empty file */
    for (i = 0; i < file->body.nitems; i++) {
        if (i) {
            put(&b, "\n");
        }
        dump_item(&b, file->body.items[i]);
    }
    return b.data;
}
