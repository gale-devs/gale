#ifndef GALE_UNFOLD_H
#define GALE_UNFOLD_H

#include <stddef.h>

#include "arena.h"
#include "ast.h"

/* `unfold a.b.c` pulls the code behind the path into the current struct, keeping
 * the path: the result is `struct a { struct b { struct c { ... } } }`, merged with
 * any struct of the same name that is already there. See docs/spec.md. */

typedef struct {
    char file[512]; /* file in which the problem was found */
    char msg[400];
    int line, col;
} UnfoldError;

typedef struct Unfolder Unfolder;

/* `roots` are the directories searched, in order, for the files behind a path
 * (`a.b.c` is looked for as <root>/a/b/c.gale, then <root>/a/b.gale, then
 * <root>/a.gale). The strings must outlive the Unfolder. */
Unfolder *unfold_new(Arena *arena, const char *const *roots, size_t nroots);

/* Expands every `unfold` in `file` (and, recursively, in the files it unfolds), in
 * place. `entry_path` is the path of `file` itself, used in messages and to detect
 * cycles. Returns 1 on success, 0 on failure with *err filled in.
 *
 * The source text of unfolded files is owned by the Unfolder: keep it alive as
 * long as the tree is used. */
int unfold_expand(Unfolder *u, File *file, const char *entry_path, UnfoldError *err);

void unfold_free(Unfolder *u);

#endif /* GALE_UNFOLD_H */
