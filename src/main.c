#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "arena.h"
#include "asm.h"
#include "ast.h"
#include "codegen.h"
#include "elf.h"
#include "encode.h"
#include "fsutil.h"
#include "lexer.h"
#include "parser.h"
#include "pe.h"
#include "prelude.h"
#include "sema.h"
#include "unfold.h"

#define GALE_VERSION "0.0.1"
#define MAX_INCLUDES 32

static void usage(FILE *out) {
    fprintf(out,
            "usage:\n"
            "  gale lex <file>                     print the token stream\n"
            "  gale parse <file>                   print the syntax tree\n"
            "  gale expand [-I dir]... <file>      print the tree with `unfold`s expanded\n"
            "  gale check [-I dir]... <file>       expand `unfold`s and type-check\n"
            "  gale asm [-I dir]... [--target t] <file>\n"
            "                                      print the generated assembly\n"
            "  gale build [-I dir]... [--target linux|windows] <file> [-o out]\n"
            "                                      compile to an x86-64 executable (ELF or PE;\n"
            "                                      the default is the system you run on)\n"
            "  gale --version | --help\n");
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

/* ---- the commands that run the compiler ---- */

typedef struct {
    const char *path;
    const char *out;
    const char *includes[MAX_INCLUDES];
    size_t nincludes;
    Target target;
} Options;

#define OPT_OUT 1    /* -o */
#define OPT_TARGET 2 /* --target */

/* [-I dir]... [--target t] <file> [-o out] */
static int parse_options(int argc, char **argv, Options *o, int allowed) {
    int a;

    memset(o, 0, sizeof *o);
    o->target = host_target();
    for (a = 0; a < argc; a++) {
        if (strcmp(argv[a], "-I") == 0) {
            if (a + 1 >= argc || o->nincludes >= MAX_INCLUDES) {
                fprintf(stderr, "gale: -I needs a directory (at most %d)\n", MAX_INCLUDES);
                return 0;
            }
            o->includes[o->nincludes++] = argv[++a];
        } else if ((allowed & OPT_TARGET) && strcmp(argv[a], "--target") == 0) {
            if (a + 1 >= argc) {
                fprintf(stderr, "gale: --target needs linux or windows\n");
                return 0;
            }
            a++;
            if (strcmp(argv[a], "linux") == 0) {
                o->target = TARGET_LINUX;
            } else if (strcmp(argv[a], "windows") == 0) {
                o->target = TARGET_WINDOWS;
            } else {
                fprintf(stderr, "gale: unknown target '%s' (use linux or windows)\n", argv[a]);
                return 0;
            }
        } else if ((allowed & OPT_OUT) && strcmp(argv[a], "-o") == 0) {
            if (a + 1 >= argc) {
                fprintf(stderr, "gale: -o needs a file name\n");
                return 0;
            }
            o->out = argv[++a];
        } else if (!o->path) {
            o->path = argv[a];
        } else {
            usage(stderr);
            return 0;
        }
    }
    if (!o->path) {
        usage(stderr);
        return 0;
    }
    return 1;
}

typedef enum {
    STAGE_EXPAND, /* parse and expand `unfold`s */
    STAGE_CHECK,  /* ... and type-check */
    STAGE_ASM,    /* ... and generate assembly */
    STAGE_BUILD   /* ... and encode and write an executable */
} Stage;

/* Runs the compiler up to `stage`. Prints errors as file:line:col: error: message.
 * Returns 0 on success, 1 on a compile error. */
static int run_compiler(const Options *o, Stage stage, int print_tree) {
    const char *roots[MAX_INCLUDES + 2];
    size_t nroots = 0, i;
    char *src, *dir;
    Arena arena;
    ParseError perr;
    UnfoldError uerr;
    SemaError serr;
    CodegenError cerr;
    File *file;
    Unfolder *u;
    Sema *sema = NULL;
    Asm as;
    int rc = 1;

    src = read_file(o->path);
    if (!src) {
        fprintf(stderr, "gale: cannot read '%s'\n", o->path);
        return 1;
    }
    /* the directory of the file itself is searched first, then the -I directories */
    dir = path_dir(o->path);
    roots[nroots++] = dir;
    for (i = 0; i < o->nincludes; i++) {
        roots[nroots++] = o->includes[i];
    }
    /* GALE_HOME: a directory that holds `std/` (so `unfold std.console` works anywhere) */
    if (getenv("GALE_HOME") && getenv("GALE_HOME")[0]) {
        roots[nroots++] = getenv("GALE_HOME");
    }

    arena_init(&arena);
    file = parse_file(src, &arena, &perr);
    if (!file) {
        fprintf(stderr, "%s:%d:%d: error: %s\n", o->path, perr.line, perr.col, perr.msg);
        goto done_noun;
    }
    u = unfold_new(&arena, roots, nroots);
    if (!unfold_expand(u, file, o->path, &uerr)) {
        fprintf(stderr, "%s:%d:%d: error: %s\n", uerr.file[0] ? uerr.file : o->path, uerr.line,
                uerr.col, uerr.msg);
        goto done;
    }
    if (stage == STAGE_EXPAND) {
        if (print_tree) {
            char *dump = ast_dump(file);
            puts(dump);
            free(dump);
        }
        rc = 0;
        goto done;
    }

    if (!prelude_add(file, &arena)) {
        fprintf(stderr, "gale: internal error: cannot parse the built-in declarations\n");
        goto done;
    }
    sema = sema_new(&arena);
    if (!sema_check(sema, file, &serr)) {
        fprintf(stderr, "%s:%d:%d: error: %s\n", serr.file[0] ? serr.file : o->path, serr.line,
                serr.col, serr.msg);
        goto done;
    }
    if (stage == STAGE_CHECK) {
        puts("ok");
        rc = 0;
        goto done;
    }

    asm_init(&as, &arena);
    {
        int entry;
        if (!codegen(sema, &as, o->target, &entry, &cerr)) {
            fprintf(stderr, "%s:%d:%d: error: %s\n", cerr.file[0] ? cerr.file : o->path, cerr.line,
                    cerr.col, cerr.msg);
            goto done;
        }
        if (stage == STAGE_ASM) {
            char *text = asm_format(&as);
            fputs(text, stdout);
            free(text);
            rc = 0;
            goto done;
        }
        {
            Code code;
            size_t *label_off = arena_alloc(&arena, (asm_label_count(&as) + 1) * sizeof(size_t));
            char err[300];
            size_t elf_len;
            unsigned char *elf;
            const char *out = o->out ? o->out : (o->target == TARGET_WINDOWS ? "a.exe" : "a.out");

            if (!asm_encode(&as, &arena, &code, label_off, err, sizeof err)) {
                fprintf(stderr, "gale: internal error: %s\n", err);
                goto done;
            }
            if (o->target == TARGET_WINDOWS) {
                elf = pe_build(&as, &code, label_off[entry], &elf_len);
            } else {
                elf = elf_build(code.data, code.len, label_off[entry], &elf_len);
            }
            if (!elf) {
                fprintf(stderr, "gale: out of memory\n");
                goto done;
            }
            if (!write_file(out, elf, elf_len, 1)) {
                fprintf(stderr, "gale: cannot write '%s'\n", out);
                free(elf);
                goto done;
            }
            free(elf);
            rc = 0;
        }
    }

done:
    if (sema) {
        sema_free(sema);
    }
    unfold_free(u);
done_noun:
    arena_free(&arena);
    free(dir);
    free(src);
    return rc;
}

static int cmd_compile(int argc, char **argv, Stage stage) {
    Options o;

    int allowed = stage == STAGE_BUILD ? OPT_OUT | OPT_TARGET : stage == STAGE_ASM ? OPT_TARGET : 0;

    if (!parse_options(argc, argv, &o, allowed)) {
        return 2;
    }
    return run_compiler(&o, stage, stage == STAGE_EXPAND);
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
        return cmd_compile(argc - 2, argv + 2, STAGE_EXPAND);
    }
    if (argc >= 3 && strcmp(argv[1], "check") == 0) {
        return cmd_compile(argc - 2, argv + 2, STAGE_CHECK);
    }
    if (argc >= 3 && strcmp(argv[1], "asm") == 0) {
        return cmd_compile(argc - 2, argv + 2, STAGE_ASM);
    }
    if (argc >= 3 && strcmp(argv[1], "build") == 0) {
        return cmd_compile(argc - 2, argv + 2, STAGE_BUILD);
    }
    usage(stderr);
    return 2;
}
