#ifndef GALE_LEXER_H
#define GALE_LEXER_H

#include <stddef.h>

typedef enum {
    TOK_EOF,
    TOK_ERROR,

    /* literals and names */
    TOK_IDENT,
    TOK_INT,
    TOK_FLOAT,
    TOK_STRING,

    /* keywords */
    TOK_KW_UNFOLD,
    TOK_KW_EXPORT,
    TOK_KW_FUNC,
    TOK_KW_STRUCT,
    TOK_KW_LET,
    TOK_KW_VAR,
    TOK_KW_IF,
    TOK_KW_ELSE,
    TOK_KW_WHILE,
    TOK_KW_DEFER,
    TOK_KW_RETURN,
    TOK_KW_TRY,
    TOK_KW_AS,
    TOK_KW_TRUE,
    TOK_KW_FALSE,

    /* brackets and separators */
    TOK_LPAREN,
    TOK_RPAREN,
    TOK_LBRACE,
    TOK_RBRACE,
    TOK_LBRACKET,
    TOK_RBRACKET,
    TOK_COMMA,
    TOK_COLON,
    TOK_SEMI,
    TOK_DOT,
    TOK_ARROW,

    /* operators */
    TOK_ASSIGN,
    TOK_EQ,
    TOK_NE,
    TOK_LT,
    TOK_LE,
    TOK_GT,
    TOK_GE,
    TOK_PLUS,
    TOK_MINUS,
    TOK_STAR,
    TOK_SLASH,
    TOK_PERCENT,
    TOK_AMP,
    TOK_PIPE,
    TOK_CARET,
    TOK_TILDE,
    TOK_BANG,
    TOK_QUESTION,
    TOK_ANDAND,
    TOK_OROR,
    TOK_SHL,
    TOK_SHR
} TokenKind;

typedef struct {
    TokenKind kind;
    const char *start; /* points into the source buffer (not NUL-terminated) */
    size_t len;
    int line;          /* 1-based */
    int col;           /* 1-based, counted in bytes */
    int nl_before;     /* 1 if a newline appears between the previous token and this one */
    const char *err;   /* message when kind == TOK_ERROR, otherwise NULL */
} Token;

typedef struct {
    const char *cur;
    int line;
    int col;
} Lexer;

/* src must be NUL-terminated and must outlive all tokens produced from it. */
void lexer_init(Lexer *lx, const char *src);

/* Returns the next token. After TOK_EOF it keeps returning TOK_EOF. */
Token lexer_next(Lexer *lx);

const char *token_kind_name(TokenKind kind);

#endif /* GALE_LEXER_H */
