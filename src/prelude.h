#ifndef GALE_PRELUDE_H
#define GALE_PRELUDE_H

#include "arena.h"
#include "ast.h"

/* The marker stored in Item.file for declarations the compiler provides itself. */
#define PRELUDE_FILE "<builtin>"

/* Adds the compiler's built-in declarations (the `builtin` struct, whose functions the
 * code generator implements) to the file. Returns 1 on success. */
int prelude_add(File *file, Arena *arena);

#endif /* GALE_PRELUDE_H */
