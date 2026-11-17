#ifndef GALE_AST_H
#define GALE_AST_H

#include <stddef.h>

#include "lexer.h"

/* All AST nodes are allocated in an Arena. Strings point into the source buffer. */

typedef struct {
    const char *ptr;
    size_t len;
} Str;

/* a.b.c */
typedef struct {
    Str *segs;
    size_t count;
} Path;

typedef struct Type Type;
typedef struct Expr Expr;
typedef struct Item Item;

/* ---- types ---- */

typedef enum {
    TYPE_PATH,     /* i32, std.mem.Allocator */
    TYPE_PTR,      /* *T */
    TYPE_OPTIONAL, /* ?T */
    TYPE_SLICE,    /* []T */
    TYPE_ARRAY,    /* [N]T */
    TYPE_ERRUNION  /* T! */
} TypeKind;

struct Type {
    TypeKind kind;
    int line, col;
    union {
        Path path;
        Type *elem; /* PTR, OPTIONAL, SLICE, ERRUNION */
        struct {
            Expr *len;
            Type *elem;
        } array;
    } u;
};

/* ---- expressions ---- */

typedef enum {
    EXPR_INT,
    EXPR_FLOAT,
    EXPR_STRING, /* lit includes the quotes and the raw escapes */
    EXPR_BOOL,
    EXPR_IDENT,
    EXPR_UNARY,  /* op: TOK_MINUS TOK_BANG TOK_TILDE TOK_STAR(deref) TOK_AMP(address-of) TOK_KW_TRY */
    EXPR_BINARY, /* op: a binary operator token */
    EXPR_ASSIGN,
    EXPR_CALL,
    EXPR_INDEX,
    EXPR_FIELD,
    EXPR_CAST,
    EXPR_BLOCK,
    EXPR_IF,
    EXPR_WHILE,
    EXPR_STRUCT_LIT
} ExprKind;

typedef enum {
    STMT_LET,
    STMT_VAR,
    STMT_DEFER,
    STMT_RETURN,
    STMT_EXPR
} StmtKind;

typedef struct {
    StmtKind kind;
    int line, col;
    Str name;   /* LET, VAR */
    Type *type; /* LET, VAR: optional */
    Expr *expr; /* LET, VAR, DEFER: required; RETURN: optional; EXPR: required */
} Stmt;

typedef struct {
    Stmt **stmts;
    size_t nstmts;
    Expr *tail; /* value of the block, or NULL (void) */
} Block;

typedef struct {
    Str name;
    int line, col;
    Expr *value;
} FieldInit;

struct Expr {
    ExprKind kind;
    int line, col;
    union {
        Str lit;      /* INT, FLOAT, STRING, IDENT */
        int boolean;  /* BOOL */
        struct {
            TokenKind op;
            Expr *operand;
        } unary;
        struct {
            TokenKind op;
            Expr *lhs, *rhs;
        } binary;     /* also ASSIGN (op is TOK_ASSIGN) */
        struct {
            Expr *callee;
            Expr **args;
            size_t nargs;
        } call;
        struct {
            Expr *base;
            Expr *index;
        } index;
        struct {
            Expr *base;
            Str name;
        } field;
        struct {
            Expr *operand;
            Type *type;
        } cast;
        Block *block;
        struct {
            Expr *cond;
            Expr *then_block; /* EXPR_BLOCK */
            Expr *else_expr;  /* NULL, EXPR_BLOCK, or EXPR_IF */
        } if_;
        struct {
            Expr *cond;
            Expr *body; /* EXPR_BLOCK */
        } while_;
        struct {
            Path path;
            FieldInit **fields;
            size_t nfields;
        } struct_lit;
    } u;
};

/* ---- items ---- */

typedef struct {
    Str name;
    int line, col;
    Type *type;
} Param;

typedef struct {
    Str name;
    int line, col;
    Type *type;
} Field;

/* The members of a struct. A source file is also a Body (items only). */
typedef struct {
    Field **fields;
    size_t nfields;
    Item **items;
    size_t nitems;
} Body;

typedef enum {
    ITEM_UNFOLD,
    ITEM_FUNC,
    ITEM_STRUCT
} ItemKind;

struct Item {
    ItemKind kind;
    int exported;
    int line, col;
    union {
        Path unfold;
        struct {
            Str name;
            Param **params;
            size_t nparams;
            Type *ret;  /* NULL means void */
            Expr *body;
        } func;
        struct {
            Str name;
            Body body;
        } struct_;
    } u;
};

typedef struct {
    Body body;
} File;

/* Returns a malloc'ed S-expression dump of the tree. The caller frees it. */
char *ast_dump(const File *file);

#endif /* GALE_AST_H */
