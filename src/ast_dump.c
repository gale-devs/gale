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
    case ITEM_FUNC:
        put(b, "func ?");
        break;
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
