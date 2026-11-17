#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "arena.h"
#include "ast.h"
#include "lexer.h"
#include "parser.h"

#define GALE_VERSION "0.0.1"

static char *read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    long size;
    char *buf;

    if (!f) {
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    size = ftell(f);
    if (size < 0 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return NULL;
    }
    buf = malloc((size_t)size + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    if (fread(buf, 1, (size_t)size, f) != (size_t)size) {
        free(buf);
        fclose(f);
        return NULL;
    }
    buf[size] = '\0';
    fclose(f);
    return buf;
}

static int cmd_lex(const char *path) {
    char *src = read_file(path);
    Lexer lx;
    int errors = 0;

    if (!src) {
        fprintf(stderr, "gale: cannot read '%s'\n", path);
        return 1;
    }
    lexer_init(&lx, src);
    for (;;) {
        Token t = lexer_next(&lx);
        if (t.kind == TOK_EOF) {
            break;
        }
        if (t.kind == TOK_ERROR) {
            fprintf(stderr, "%s:%d:%d: error: %s\n", path, t.line, t.col, t.err);
            errors++;
            continue;
        }
        printf("%d:%d\t%s\t%.*s\n", t.line, t.col, token_kind_name(t.kind), (int)t.len, t.start);
    }
    free(src);
    return errors ? 1 : 0;
}

static int cmd_parse(const char *path) {
    char *src = read_file(path);
    Arena arena;
    ParseError err;
    File *file;
    char *dump;

    if (!src) {
        fprintf(stderr, "gale: cannot read '%s'\n", path);
        return 1;
    }
    arena_init(&arena);
    file = parse_file(src, &arena, &err);
    if (!file) {
        fprintf(stderr, "%s:%d:%d: error: %s\n", path, err.line, err.col, err.msg);
        arena_free(&arena);
        free(src);
        return 1;
    }
    dump = ast_dump(file);
    puts(dump);
    free(dump);
    arena_free(&arena);
    free(src);
    return 0;
}

static void usage(FILE *out) {
    fprintf(out,
            "usage:\n"
            "  gale lex <file>    print the token stream\n"
            "  gale parse <file>  print the syntax tree\n"
            "  gale --version\n"
            "  gale --help\n");
}

int main(int argc, char **argv) {
    if (argc == 2 && strcmp(argv[1], "--version") == 0) {
        printf("gale %s\n", GALE_VERSION);
        return 0;
    }
    if (argc == 2 && (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0)) {
        usage(stdout);
        return 0;
    }
    if (argc == 3 && strcmp(argv[1], "lex") == 0) {
        return cmd_lex(argv[2]);
    }
    if (argc == 3 && strcmp(argv[1], "parse") == 0) {
        return cmd_parse(argv[2]);
    }
    usage(stderr);
    return 2;
}
