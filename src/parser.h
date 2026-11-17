#ifndef GALE_PARSER_H
#define GALE_PARSER_H

#include "arena.h"
#include "ast.h"

typedef struct {
    char msg[200];
    int line, col;
} ParseError;

/* Parses a NUL-terminated source buffer.
 *
 * On success returns the tree; every node lives in `arena` and strings point
 * into `src`, so both must outlive the tree. On failure returns NULL and fills
 * `*err` (the first error only). */
File *parse_file(const char *src, Arena *arena, ParseError *err);

#endif /* GALE_PARSER_H */
