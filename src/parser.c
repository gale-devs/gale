#include "parser.h"

#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#if defined(_MSC_VER)
#define NORETURN __declspec(noreturn)
#elif defined(__GNUC__)
#define NORETURN __attribute__((noreturn))
#else
#define NORETURN
#endif

typedef struct {
    Arena *arena;
    Token *toks; /* all tokens, ending with TOK_EOF */
    size_t ntoks;
    size_t pos;
    int no_struct; /* 1 while parsing an if/while condition: `Name {` is not a struct literal */
    ParseError *err;
    jmp_buf jb;
} Parser;

/* ---- token helpers ---- */

static Token *cur(Parser *p) {
    return &p->toks[p->pos];
}

static Token *peek_at(Parser *p, size_t n) {
    size_t i = p->pos + n;
    if (i >= p->ntoks) {
        i = p->ntoks - 1;
    }
    return &p->toks[i];
}

static int at(Parser *p, TokenKind k) {
    return cur(p)->kind == k;
}

static Token *next(Parser *p) {
    Token *t = cur(p);
    if (t->kind != TOK_EOF) {
        p->pos++;
    }
    return t;
}

static int eat(Parser *p, TokenKind k) {
    if (at(p, k)) {
        p->pos++;
        return 1;
    }
    return 0;
}

static NORETURN void fail(Parser *p, const Token *t, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(p->err->msg, sizeof p->err->msg, fmt, ap);
    va_end(ap);
    p->err->line = t->line;
    p->err->col = t->col;
    longjmp(p->jb, 1);
}

static NORETURN void fail_expected(Parser *p, const char *what) {
    const Token *t = cur(p);
    if (t->kind == TOK_EOF) {
        fail(p, t, "expected %s, found end of file", what);
    }
    fail(p, t, "expected %s, found '%.*s'", what, (int)(t->len > 40 ? 40 : t->len), t->start);
}

static Token *expect(Parser *p, TokenKind k, const char *what) {
    if (!at(p, k)) {
        fail_expected(p, what);
    }
    return next(p);
}

static Str tok_str(const Token *t) {
    Str s;
    s.ptr = t->start;
    s.len = t->len;
    return s;
}

static Str expect_ident(Parser *p, const char *what) {
    return tok_str(expect(p, TOK_IDENT, what));
}

/* ---- paths ---- */

static Path parse_path(Parser *p) {
    Vec v;
    Path path;
    Str s;

    vec_init(&v, sizeof(Str));
    s = expect_ident(p, "a name");
    vec_push(p->arena, &v, &s);
    while (at(p, TOK_DOT) && peek_at(p, 1)->kind == TOK_IDENT) {
        next(p);
        s = expect_ident(p, "a name");
        vec_push(p->arena, &v, &s);
    }
    path.segs = v.data;
    path.count = v.len;
    return path;
}

/* ---- expressions (grown in later steps) ---- */

static Expr *new_expr(Parser *p, ExprKind kind, const Token *t) {
    Expr *e = arena_alloc(p->arena, sizeof(Expr));
    e->kind = kind;
    e->line = t->line;
    e->col = t->col;
    return e;
}

static Expr *parse_expr(Parser *p);
static Expr *parse_block(Parser *p);
static Expr *parse_if(Parser *p);
static Expr *parse_while(Parser *p);
static Type *parse_type_prefix(Parser *p);
static Type *parse_type(Parser *p);

static Expr *parse_primary(Parser *p) {
    Token *t = cur(p);
    Expr *e;

    switch (t->kind) {
    case TOK_INT:
        e = new_expr(p, EXPR_INT, t);
        e->u.lit = tok_str(t);
        next(p);
        return e;
    case TOK_FLOAT:
        e = new_expr(p, EXPR_FLOAT, t);
        e->u.lit = tok_str(t);
        next(p);
        return e;
    case TOK_STRING:
        e = new_expr(p, EXPR_STRING, t);
        e->u.lit = tok_str(t);
        next(p);
        return e;
    case TOK_KW_TRUE:
    case TOK_KW_FALSE:
        e = new_expr(p, EXPR_BOOL, t);
        e->u.boolean = (t->kind == TOK_KW_TRUE);
        next(p);
        return e;
    case TOK_IDENT:
        e = new_expr(p, EXPR_IDENT, t);
        e->u.lit = tok_str(t);
        next(p);
        return e;
    case TOK_LPAREN: {
        int saved = p->no_struct;
        next(p);
        p->no_struct = 0;
        e = parse_expr(p);
        p->no_struct = saved;
        expect(p, TOK_RPAREN, "')'");
        return e;
    }
    case TOK_LBRACE:
        return parse_block(p);
    case TOK_KW_IF:
        return parse_if(p);
    case TOK_KW_WHILE:
        return parse_while(p);
    default:
        fail_expected(p, "an expression");
    }
}

/* Postfix operators must start on the same line as the expression they apply to,
 * so a line beginning with '(' or '[' starts a new statement. */
static Expr *parse_postfix(Parser *p) {
    Expr *e = parse_primary(p);

    for (;;) {
        Token *t = cur(p);
        Expr *n;

        if (t->nl_before) {
            break;
        }
        if (t->kind == TOK_LPAREN) {
            Vec args;
            int saved = p->no_struct;

            next(p);
            p->no_struct = 0;
            vec_init(&args, sizeof(Expr *));
            while (!at(p, TOK_RPAREN)) {
                Expr *a = parse_expr(p);
                vec_push(p->arena, &args, &a);
                if (!eat(p, TOK_COMMA)) {
                    break;
                }
            }
            p->no_struct = saved;
            expect(p, TOK_RPAREN, "')'");
            n = new_expr(p, EXPR_CALL, t);
            n->u.call.callee = e;
            n->u.call.args = args.data;
            n->u.call.nargs = args.len;
        } else if (t->kind == TOK_LBRACKET) {
            int saved = p->no_struct;

            next(p);
            p->no_struct = 0;
            n = new_expr(p, EXPR_INDEX, t);
            n->u.index.base = e;
            n->u.index.index = parse_expr(p);
            p->no_struct = saved;
            expect(p, TOK_RBRACKET, "']'");
        } else if (t->kind == TOK_DOT) {
            next(p);
            n = new_expr(p, EXPR_FIELD, t);
            n->u.field.base = e;
            n->u.field.name = expect_ident(p, "a field name");
        } else {
            break;
        }
        n->line = e->line;
        n->col = e->col;
        e = n;
    }
    return e;
}

static Expr *parse_unary(Parser *p) {
    Token *t = cur(p);

    switch (t->kind) {
    case TOK_MINUS:
    case TOK_BANG:
    case TOK_TILDE:
    case TOK_STAR:
    case TOK_AMP:
    case TOK_KW_TRY: {
        Expr *e = new_expr(p, EXPR_UNARY, t);
        next(p);
        e->u.unary.op = t->kind;
        e->u.unary.operand = parse_unary(p);
        return e;
    }
    default:
        return parse_postfix(p);
    }
}

/* `expr as T` binds tighter than the binary operators and looser than unary ones. */
static Expr *parse_cast(Parser *p) {
    Expr *e = parse_unary(p);

    while (at(p, TOK_KW_AS) && !cur(p)->nl_before) {
        Expr *c = new_expr(p, EXPR_CAST, cur(p));
        next(p);
        c->line = e->line;
        c->col = e->col;
        c->u.cast.operand = e;
        c->u.cast.type = parse_type_prefix(p);
        e = c;
    }
    return e;
}

static int binary_prec(TokenKind k) {
    switch (k) {
    case TOK_OROR:
        return 1;
    case TOK_ANDAND:
        return 2;
    case TOK_EQ:
    case TOK_NE:
    case TOK_LT:
    case TOK_LE:
    case TOK_GT:
    case TOK_GE:
        return 3;
    case TOK_PIPE:
        return 4;
    case TOK_CARET:
        return 5;
    case TOK_AMP:
        return 6;
    case TOK_SHL:
    case TOK_SHR:
        return 7;
    case TOK_PLUS:
    case TOK_MINUS:
        return 8;
    case TOK_STAR:
    case TOK_SLASH:
    case TOK_PERCENT:
        return 9;
    default:
        return 0;
    }
}

/* Binary operators are left-associative. An operator must be on the same line as
 * its left operand (a line may end with an operator, but not start with one). */
static Expr *parse_binary(Parser *p, int min_prec) {
    Expr *lhs = parse_cast(p);

    for (;;) {
        Token *t = cur(p);
        int prec;
        Expr *e;

        if (t->nl_before) {
            break;
        }
        prec = binary_prec(t->kind);
        if (prec == 0 || prec < min_prec) {
            break;
        }
        next(p);
        e = new_expr(p, EXPR_BINARY, t);
        e->line = lhs->line;
        e->col = lhs->col;
        e->u.binary.op = t->kind;
        e->u.binary.lhs = lhs;
        e->u.binary.rhs = parse_binary(p, prec + 1);
        lhs = e;
    }
    return lhs;
}

/* Assignment is an expression, right-associative, with the lowest precedence. */
static Expr *parse_expr(Parser *p) {
    Expr *lhs = parse_binary(p, 1);

    if (at(p, TOK_ASSIGN) && !cur(p)->nl_before) {
        Token *t = next(p);
        Expr *e = new_expr(p, EXPR_ASSIGN, t);
        e->line = lhs->line;
        e->col = lhs->col;
        e->u.binary.op = TOK_ASSIGN;
        e->u.binary.lhs = lhs;
        e->u.binary.rhs = parse_expr(p);
        return e;
    }
    return lhs;
}

/* ---- blocks and statements ---- */

static Stmt *new_stmt(Parser *p, StmtKind kind, const Token *t) {
    Stmt *st = arena_alloc(p->arena, sizeof(Stmt));
    st->kind = kind;
    st->line = t->line;
    st->col = t->col;
    return st;
}

/* A statement ends at ';', at a newline, or right before the closing '}'. */
static void end_stmt(Parser *p) {
    if (eat(p, TOK_SEMI) || at(p, TOK_RBRACE) || cur(p)->nl_before) {
        return;
    }
    if (at(p, TOK_EOF)) {
        fail_expected(p, "'}'");
    }
    fail_expected(p, "';' or a newline");
}

/* { stmt* [expr] }  -- a final expression without ';' is the value of the block. */
static Expr *parse_block(Parser *p) {
    Token *open = expect(p, TOK_LBRACE, "'{'");
    Expr *e = new_expr(p, EXPR_BLOCK, open);
    Block *b = arena_alloc(p->arena, sizeof(Block));
    Vec stmts;
    int saved = p->no_struct;
    int done = 0;

    vec_init(&stmts, sizeof(Stmt *));
    p->no_struct = 0;

    while (!done && !at(p, TOK_RBRACE)) {
        Token *t = cur(p);
        Stmt *st = NULL;

        if (t->kind == TOK_EOF) {
            fail_expected(p, "'}'");
        }
        if (eat(p, TOK_SEMI)) {
            continue;
        }

        switch (t->kind) {
        case TOK_KW_LET:
        case TOK_KW_VAR:
            next(p);
            st = new_stmt(p, t->kind == TOK_KW_LET ? STMT_LET : STMT_VAR, t);
            st->name = expect_ident(p, "a variable name");
            if (eat(p, TOK_COLON)) {
                st->type = parse_type(p);
            }
            expect(p, TOK_ASSIGN, "'='");
            st->expr = parse_expr(p);
            end_stmt(p);
            break;
        case TOK_KW_DEFER:
            next(p);
            st = new_stmt(p, STMT_DEFER, t);
            st->expr = parse_expr(p);
            end_stmt(p);
            break;
        case TOK_KW_RETURN:
            next(p);
            st = new_stmt(p, STMT_RETURN, t);
            if (!(at(p, TOK_SEMI) || at(p, TOK_RBRACE) || at(p, TOK_EOF) || cur(p)->nl_before)) {
                st->expr = parse_expr(p);
            }
            end_stmt(p);
            break;
        default: {
            Expr *x = parse_expr(p);
            if (at(p, TOK_RBRACE)) {
                b->tail = x;
                done = 1;
            } else {
                st = new_stmt(p, STMT_EXPR, t);
                st->expr = x;
                end_stmt(p);
            }
            break;
        }
        }
        if (st) {
            vec_push(p->arena, &stmts, &st);
        }
    }

    p->no_struct = saved;
    expect(p, TOK_RBRACE, "'}'");
    b->stmts = stmts.data;
    b->nstmts = stmts.len;
    e->u.block = b;
    return e;
}

/* The condition of if/while is parsed with struct literals disabled, so that
 * `if x { ... }` is not read as the literal `x { ... }`. Wrap it in parentheses
 * to use a struct literal there. */
static Expr *parse_condition(Parser *p) {
    int saved = p->no_struct;
    Expr *cond;

    p->no_struct = 1;
    cond = parse_expr(p);
    p->no_struct = saved;
    return cond;
}

/* if cond { ... } [else if ... | else { ... }]   (`else` may start a new line) */
static Expr *parse_if(Parser *p) {
    Token *t = expect(p, TOK_KW_IF, "'if'");
    Expr *e = new_expr(p, EXPR_IF, t);

    e->u.if_.cond = parse_condition(p);
    e->u.if_.then_block = parse_block(p);
    if (eat(p, TOK_KW_ELSE)) {
        if (at(p, TOK_KW_IF)) {
            e->u.if_.else_expr = parse_if(p);
        } else {
            e->u.if_.else_expr = parse_block(p);
        }
    }
    return e;
}

static Expr *parse_while(Parser *p) {
    Token *t = expect(p, TOK_KW_WHILE, "'while'");
    Expr *e = new_expr(p, EXPR_WHILE, t);

    e->u.while_.cond = parse_condition(p);
    e->u.while_.body = parse_block(p);
    return e;
}

/* ---- types ---- */

static Type *new_type(Parser *p, TypeKind kind, const Token *t) {
    Type *ty = arena_alloc(p->arena, sizeof(Type));
    ty->kind = kind;
    ty->line = t->line;
    ty->col = t->col;
    return ty;
}

static Type *parse_type_prefix(Parser *p) {
    Token *t = cur(p);
    Type *ty;

    if (eat(p, TOK_STAR)) {
        ty = new_type(p, TYPE_PTR, t);
        ty->u.elem = parse_type_prefix(p);
    } else if (eat(p, TOK_QUESTION)) {
        ty = new_type(p, TYPE_OPTIONAL, t);
        ty->u.elem = parse_type_prefix(p);
    } else if (eat(p, TOK_LBRACKET)) {
        if (eat(p, TOK_RBRACKET)) {
            ty = new_type(p, TYPE_SLICE, t);
            ty->u.elem = parse_type_prefix(p);
        } else {
            int saved = p->no_struct;
            ty = new_type(p, TYPE_ARRAY, t);
            p->no_struct = 0;
            ty->u.array.len = parse_expr(p);
            p->no_struct = saved;
            expect(p, TOK_RBRACKET, "']'");
            ty->u.array.elem = parse_type_prefix(p);
        }
    } else if (at(p, TOK_IDENT)) {
        ty = new_type(p, TYPE_PATH, t);
        ty->u.path = parse_path(p);
    } else {
        fail_expected(p, "a type");
    }
    return ty;
}

/* T! (error union) applies to the whole prefix type. It must be on the same line. */
static Type *parse_type(Parser *p) {
    Type *ty = parse_type_prefix(p);
    if (at(p, TOK_BANG) && !cur(p)->nl_before) {
        Type *e = new_type(p, TYPE_ERRUNION, cur(p));
        next(p);
        e->line = ty->line;
        e->col = ty->col;
        e->u.elem = ty;
        ty = e;
    }
    return ty;
}

/* ---- items ---- */

static Item *parse_item(Parser *p);

/* Members of a struct body (or of the file when end == TOK_EOF). Members are
 * separated by ',' or ';' or a newline. */
static void parse_body(Parser *p, Body *body, int allow_fields, TokenKind end) {
    Vec fields, items;

    vec_init(&fields, sizeof(Field *));
    vec_init(&items, sizeof(Item *));

    while (!at(p, end)) {
        Token *t = cur(p);

        if (t->kind == TOK_EOF) {
            fail_expected(p, "'}'");
        }
        if (eat(p, TOK_SEMI) || eat(p, TOK_COMMA)) {
            continue;
        }

        if (t->kind == TOK_IDENT && peek_at(p, 1)->kind == TOK_COLON) {
            Field *f;
            if (!allow_fields) {
                fail(p, t, "fields are not allowed at file level");
            }
            f = arena_alloc(p->arena, sizeof(Field));
            f->name = tok_str(t);
            f->line = t->line;
            f->col = t->col;
            next(p);
            next(p);
            f->type = parse_type(p);
            vec_push(p->arena, &fields, &f);
        } else if (t->kind == TOK_KW_EXPORT || t->kind == TOK_KW_UNFOLD ||
                   t->kind == TOK_KW_FUNC || t->kind == TOK_KW_STRUCT) {
            Item *it = parse_item(p);
            vec_push(p->arena, &items, &it);
        } else {
            fail_expected(p, allow_fields ? "a field or an item" : "an item");
        }

        if (!(eat(p, TOK_SEMI) || eat(p, TOK_COMMA) || at(p, end) || cur(p)->nl_before)) {
            if (at(p, TOK_EOF)) {
                fail_expected(p, "'}'");
            }
            fail_expected(p, "',' or a newline");
        }
    }

    body->fields = fields.data;
    body->nfields = fields.len;
    body->items = items.data;
    body->nitems = items.len;
}

static void parse_func(Parser *p, Item *it) {
    Vec params;

    it->kind = ITEM_FUNC;
    it->u.func.name = expect_ident(p, "a function name");
    expect(p, TOK_LPAREN, "'('");
    vec_init(&params, sizeof(Param *));
    while (!at(p, TOK_RPAREN)) {
        Token *t = cur(p);
        Param *pa = arena_alloc(p->arena, sizeof(Param));
        pa->name = expect_ident(p, "a parameter name");
        pa->line = t->line;
        pa->col = t->col;
        expect(p, TOK_COLON, "':'");
        pa->type = parse_type(p);
        vec_push(p->arena, &params, &pa);
        if (!eat(p, TOK_COMMA)) {
            break;
        }
    }
    expect(p, TOK_RPAREN, "')'");
    it->u.func.params = params.data;
    it->u.func.nparams = params.len;

    if (eat(p, TOK_ARROW)) {
        it->u.func.ret = parse_type(p);
        if (at(p, TOK_NE)) {
            fail(p, cur(p), "expected '=', found '!=' (write 'T! =' with a space)");
        }
    }
    expect(p, TOK_ASSIGN, "'='");
    it->u.func.body = parse_expr(p);
}

static Item *parse_item(Parser *p) {
    Token *start = cur(p);
    Item *it = arena_alloc(p->arena, sizeof(Item));

    it->line = start->line;
    it->col = start->col;
    it->exported = eat(p, TOK_KW_EXPORT);

    switch (cur(p)->kind) {
    case TOK_KW_UNFOLD:
        next(p);
        it->kind = ITEM_UNFOLD;
        it->u.unfold = parse_path(p);
        break;
    case TOK_KW_FUNC:
        next(p);
        parse_func(p, it);
        break;
    case TOK_KW_STRUCT:
        next(p);
        it->kind = ITEM_STRUCT;
        it->u.struct_.name = expect_ident(p, "a struct name");
        expect(p, TOK_ASSIGN, "'='");
        expect(p, TOK_LBRACE, "'{'");
        parse_body(p, &it->u.struct_.body, 1, TOK_RBRACE);
        expect(p, TOK_RBRACE, "'}'");
        break;
    default:
        fail_expected(p, "'unfold', 'func' or 'struct'");
    }
    return it;
}

/* ---- entry point ---- */

File *parse_file(const char *src, Arena *arena, ParseError *err) {
    Parser p;
    Lexer lx;
    Vec toks;
    File *file;

    memset(&p, 0, sizeof p);
    p.arena = arena;
    p.err = err;
    err->msg[0] = '\0';
    err->line = 0;
    err->col = 0;

    lexer_init(&lx, src);
    vec_init(&toks, sizeof(Token));
    for (;;) {
        Token t = lexer_next(&lx);
        if (t.kind == TOK_ERROR) {
            snprintf(err->msg, sizeof err->msg, "%s", t.err);
            err->line = t.line;
            err->col = t.col;
            return NULL;
        }
        vec_push(arena, &toks, &t);
        if (t.kind == TOK_EOF) {
            break;
        }
    }
    p.toks = toks.data;
    p.ntoks = toks.len;

    if (setjmp(p.jb)) {
        return NULL;
    }

    file = arena_alloc(arena, sizeof(File));
    parse_body(&p, &file->body, 0, TOK_EOF);
    return file;
}
