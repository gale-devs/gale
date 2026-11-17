#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "arena.h"
#include "ast.h"
#include "fsutil.h"
#include "lexer.h"
#include "parser.h"
#include "unfold.h"

#define GALE_VERSION "0.0.1"

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

static void usage(FILE *out);

#define MAX_INCLUDES 32

/* gale expand [-I dir]... <file>: parse the file and expand its `unfold`s. */
static int cmd_expand(int argc, char **argv) {
    const char *includes[MAX_INCLUDES];
    const char *roots[MAX_INCLUDES + 1];
    size_t nincludes = 0, nroots = 0, i;
    const char *path = NULL;
    char *src, *dir, *dump;
    Arena arena;
    ParseError perr;
    UnfoldError uerr;
    Unfolder *u;
    File *file;
    int rc = 1;
    int a;

    for (a = 0; a < argc; a++) {
        if (strcmp(argv[a], "-I") == 0) {
            if (a + 1 >= argc || nincludes >= MAX_INCLUDES) {
                fprintf(stderr, "gale: -I needs a directory (at most %d)\n", MAX_INCLUDES);
                return 2;
            }
            includes[nincludes++] = argv[++a];
        } else if (!path) {
            path = argv[a];
        } else {
            usage(stderr);
            return 2;
        }
    }
    if (!path) {
        usage(stderr);
        return 2;
    }

    src = read_file(path);
    if (!src) {
        fprintf(stderr, "gale: cannot read '%s'\n", path);
        return 1;
    }
    /* the directory of the file itself is searched first, then the -I directories */
    dir = path_dir(path);
    roots[nroots++] = dir;
    for (i = 0; i < nincludes; i++) {
        roots[nroots++] = includes[i];
    }

    arena_init(&arena);
    file = parse_file(src, &arena, &perr);
    if (!file) {
        fprintf(stderr, "%s:%d:%d: error: %s\n", path, perr.line, perr.col, perr.msg);
    } else {
        u = unfold_new(&arena, roots, nroots);
        if (!unfold_expand(u, file, path, &uerr)) {
            fprintf(stderr, "%s:%d:%d: error: %s\n", uerr.file[0] ? uerr.file : path, uerr.line,
                    uerr.col, uerr.msg);
        } else {
            dump = ast_dump(file);
            puts(dump);
            free(dump);
            rc = 0;
        }
        unfold_free(u);
    }
    arena_free(&arena);
    free(dir);
    free(src);
    return rc;
}

static void usage(FILE *out) {
    fprintf(out,
            "usage:\n"
            "  gale lex <file>    print the token stream\n"
            "  gale parse <file>  print the syntax tree\n"
            "  gale expand [-I dir]... <file>\n"
            "                     print the syntax tree with `unfold`s expanded\n"
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
    if (argc >= 3 && strcmp(argv[1], "expand") == 0) {
        return cmd_expand(argc - 2, argv + 2);
    }
    usage(stderr);
    return 2;
}
