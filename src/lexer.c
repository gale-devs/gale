#include "lexer.h"

#include <string.h>

typedef struct {
    const char *start;
    int line;
    int col;
    int nl;
} Origin;

static int is_alpha(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

static int is_digit(char c) {
    return c >= '0' && c <= '9';
}

static int is_hex(char c) {
    return is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

static char peek(const Lexer *lx) {
    return *lx->cur;
}

static char peek2(const Lexer *lx) {
    return lx->cur[0] ? lx->cur[1] : '\0';
}

static char advance(Lexer *lx) {
    char c = *lx->cur++;
    if (c == '\n') {
        lx->line++;
        lx->col = 1;
    } else {
        lx->col++;
    }
    return c;
}

static int match(Lexer *lx, char c) {
    if (peek(lx) != c) {
        return 0;
    }
    advance(lx);
    return 1;
}

static Token make(const Lexer *lx, TokenKind kind, const Origin *o, const char *err) {
    Token t;
    t.kind = kind;
    t.start = o->start;
    t.len = (size_t)(lx->cur - o->start);
    t.line = o->line;
    t.col = o->col;
    t.nl_before = o->nl;
    t.err = err;
    return t;
}

static const struct {
    const char *name;
    TokenKind kind;
} KEYWORDS[] = {
    {"unfold", TOK_KW_UNFOLD},
    {"export", TOK_KW_EXPORT},
    {"func", TOK_KW_FUNC},
    {"struct", TOK_KW_STRUCT},
    {"let", TOK_KW_LET},
    {"var", TOK_KW_VAR},
    {"if", TOK_KW_IF},
    {"else", TOK_KW_ELSE},
    {"while", TOK_KW_WHILE},
    {"defer", TOK_KW_DEFER},
    {"return", TOK_KW_RETURN},
    {"try", TOK_KW_TRY},
    {"as", TOK_KW_AS},
    {"true", TOK_KW_TRUE},
    {"false", TOK_KW_FALSE},
};

static TokenKind keyword_kind(const char *s, size_t len) {
    size_t i;
    for (i = 0; i < sizeof(KEYWORDS) / sizeof(KEYWORDS[0]); i++) {
        if (strlen(KEYWORDS[i].name) == len && memcmp(KEYWORDS[i].name, s, len) == 0) {
            return KEYWORDS[i].kind;
        }
    }
    return TOK_IDENT;
}

/* Skips whitespace and comments. Returns an error message or NULL. */
static const char *skip_trivia(Lexer *lx, int *nl) {
    for (;;) {
        char c = peek(lx);
        if (c == ' ' || c == '\t' || c == '\r') {
            advance(lx);
        } else if (c == '\n') {
            *nl = 1;
            advance(lx);
        } else if (c == '/' && peek2(lx) == '/') {
            while (peek(lx) && peek(lx) != '\n') {
                advance(lx);
            }
        } else if (c == '/' && peek2(lx) == '*') {
            advance(lx);
            advance(lx);
            for (;;) {
                if (!peek(lx)) {
                    return "unterminated block comment";
                }
                if (peek(lx) == '*' && peek2(lx) == '/') {
                    advance(lx);
                    advance(lx);
                    break;
                }
                if (peek(lx) == '\n') {
                    *nl = 1;
                }
                advance(lx);
            }
        } else {
            return NULL;
        }
    }
}

static Token lex_number(Lexer *lx, const Origin *o) {
    int is_float = 0;
    const char *err = NULL;

    if (peek(lx) == '0' && (peek2(lx) == 'x' || peek2(lx) == 'X')) {
        advance(lx);
        advance(lx);
        if (!is_hex(peek(lx))) {
            err = "expected hex digit after 0x";
        }
        while (is_hex(peek(lx)) || peek(lx) == '_') {
            advance(lx);
        }
    } else if (peek(lx) == '0' && (peek2(lx) == 'b' || peek2(lx) == 'B')) {
        advance(lx);
        advance(lx);
        if (peek(lx) != '0' && peek(lx) != '1') {
            err = "expected binary digit after 0b";
        }
        while (peek(lx) == '0' || peek(lx) == '1' || peek(lx) == '_') {
            advance(lx);
        }
    } else {
        while (is_digit(peek(lx)) || peek(lx) == '_') {
            advance(lx);
        }
        if (peek(lx) == '.' && is_digit(peek2(lx))) {
            is_float = 1;
            advance(lx);
            while (is_digit(peek(lx)) || peek(lx) == '_') {
                advance(lx);
            }
        }
        if (peek(lx) == 'e' || peek(lx) == 'E') {
            const char *p = lx->cur + 1;
            if (*p == '+' || *p == '-') {
                p++;
            }
            if (is_digit(*p)) {
                is_float = 1;
                advance(lx);
                if (peek(lx) == '+' || peek(lx) == '-') {
                    advance(lx);
                }
                while (is_digit(peek(lx))) {
                    advance(lx);
                }
            }
        }
    }

    if (!err && (is_alpha(peek(lx)) || is_digit(peek(lx)))) {
        err = "invalid character in number literal";
    }
    if (err) {
        while (is_alpha(peek(lx)) || is_digit(peek(lx))) {
            advance(lx);
        }
        return make(lx, TOK_ERROR, o, err);
    }
    return make(lx, is_float ? TOK_FLOAT : TOK_INT, o, NULL);
}

static Token lex_string(Lexer *lx, const Origin *o) {
    const char *err = NULL;

    advance(lx); /* opening quote */
    for (;;) {
        char c = peek(lx);
        if (c == '\0' || c == '\n') {
            if (!err) {
                err = "unterminated string literal";
            }
            break;
        }
        if (c == '"') {
            advance(lx);
            break;
        }
        if (c == '\\') {
            advance(lx);
            c = peek(lx);
            if (c == '\0' || c == '\n') {
                if (!err) {
                    err = "unterminated string literal";
                }
                break;
            }
            if (!err && !strchr("nrt0\\\"'", c)) {
                err = "invalid escape sequence";
            }
            advance(lx);
        } else {
            advance(lx);
        }
    }
    return make(lx, err ? TOK_ERROR : TOK_STRING, o, err);
}

void lexer_init(Lexer *lx, const char *src) {
    lx->cur = src;
    lx->line = 1;
    lx->col = 1;
}

Token lexer_next(Lexer *lx) {
    Origin o;
    char c;
    int nl = 0;
    const char *trivia_err = skip_trivia(lx, &nl);

    o.start = lx->cur;
    o.line = lx->line;
    o.col = lx->col;
    o.nl = nl;

    if (trivia_err) {
        return make(lx, TOK_ERROR, &o, trivia_err);
    }

    c = peek(lx);
    if (c == '\0') {
        return make(lx, TOK_EOF, &o, NULL);
    }
    if (is_alpha(c)) {
        while (is_alpha(peek(lx)) || is_digit(peek(lx))) {
            advance(lx);
        }
        return make(lx, keyword_kind(o.start, (size_t)(lx->cur - o.start)), &o, NULL);
    }
    if (is_digit(c)) {
        return lex_number(lx, &o);
    }
    if (c == '"') {
        return lex_string(lx, &o);
    }

    advance(lx);

#define TOK(k) return make(lx, (k), &o, NULL)
    switch (c) {
    case '(': TOK(TOK_LPAREN);
    case ')': TOK(TOK_RPAREN);
    case '{': TOK(TOK_LBRACE);
    case '}': TOK(TOK_RBRACE);
    case '[': TOK(TOK_LBRACKET);
    case ']': TOK(TOK_RBRACKET);
    case ',': TOK(TOK_COMMA);
    case ':': TOK(TOK_COLON);
    case ';': TOK(TOK_SEMI);
    case '.': TOK(TOK_DOT);
    case '+': TOK(TOK_PLUS);
    case '*': TOK(TOK_STAR);
    case '/': TOK(TOK_SLASH);
    case '%': TOK(TOK_PERCENT);
    case '^': TOK(TOK_CARET);
    case '~': TOK(TOK_TILDE);
    case '?': TOK(TOK_QUESTION);
    case '-': TOK(match(lx, '>') ? TOK_ARROW : TOK_MINUS);
    case '=': TOK(match(lx, '=') ? TOK_EQ : TOK_ASSIGN);
    case '!': TOK(match(lx, '=') ? TOK_NE : TOK_BANG);
    case '&': TOK(match(lx, '&') ? TOK_ANDAND : TOK_AMP);
    case '|': TOK(match(lx, '|') ? TOK_OROR : TOK_PIPE);
    case '<':
        if (match(lx, '=')) {
            TOK(TOK_LE);
        }
        if (match(lx, '<')) {
            TOK(TOK_SHL);
        }
        TOK(TOK_LT);
    case '>':
        if (match(lx, '=')) {
            TOK(TOK_GE);
        }
        if (match(lx, '>')) {
            TOK(TOK_SHR);
        }
        TOK(TOK_GT);
    default:
        break;
    }
#undef TOK

    /* Swallow the rest of a multi-byte UTF-8 sequence so it yields one error. */
    if ((unsigned char)c >= 0x80) {
        while ((unsigned char)peek(lx) >= 0x80) {
            advance(lx);
        }
    }
    return make(lx, TOK_ERROR, &o, "unexpected character");
}

const char *token_kind_name(TokenKind kind) {
#define N(k) case k: return #k
    switch (kind) {
    N(TOK_EOF);
    N(TOK_ERROR);
    N(TOK_IDENT);
    N(TOK_INT);
    N(TOK_FLOAT);
    N(TOK_STRING);
    N(TOK_KW_UNFOLD);
    N(TOK_KW_EXPORT);
    N(TOK_KW_FUNC);
    N(TOK_KW_STRUCT);
    N(TOK_KW_LET);
    N(TOK_KW_VAR);
    N(TOK_KW_IF);
    N(TOK_KW_ELSE);
    N(TOK_KW_WHILE);
    N(TOK_KW_DEFER);
    N(TOK_KW_RETURN);
    N(TOK_KW_TRY);
    N(TOK_KW_AS);
    N(TOK_KW_TRUE);
    N(TOK_KW_FALSE);
    N(TOK_LPAREN);
    N(TOK_RPAREN);
    N(TOK_LBRACE);
    N(TOK_RBRACE);
    N(TOK_LBRACKET);
    N(TOK_RBRACKET);
    N(TOK_COMMA);
    N(TOK_COLON);
    N(TOK_SEMI);
    N(TOK_DOT);
    N(TOK_ARROW);
    N(TOK_ASSIGN);
    N(TOK_EQ);
    N(TOK_NE);
    N(TOK_LT);
    N(TOK_LE);
    N(TOK_GT);
    N(TOK_GE);
    N(TOK_PLUS);
    N(TOK_MINUS);
    N(TOK_STAR);
    N(TOK_SLASH);
    N(TOK_PERCENT);
    N(TOK_AMP);
    N(TOK_PIPE);
    N(TOK_CARET);
    N(TOK_TILDE);
    N(TOK_BANG);
    N(TOK_QUESTION);
    N(TOK_ANDAND);
    N(TOK_OROR);
    N(TOK_SHL);
    N(TOK_SHR);
    }
#undef N
    return "TOK_UNKNOWN";
}
