#include "prelude.h"

#include <string.h>

#include "parser.h"

/* The functions are declared here so that the type checker knows their types; the
 * bodies are placeholders: codegen.c emits the real code for them. */
static const char PRELUDE_SRC[] =
    "struct builtin = {\n"
    "    export func write(s: []u8) = {}\n"
    "}\n";

static void tag(Body *body) {
    size_t i;
    for (i = 0; i < body->nitems; i++) {
        Item *it = body->items[i];
        it->file = PRELUDE_FILE;
        if (it->kind == ITEM_STRUCT) {
            tag(&it->u.struct_.body);
        }
    }
}

int prelude_add(File *file, Arena *arena) {
    ParseError err;
    File *p = parse_file(PRELUDE_SRC, arena, &err);
    Item **items;
    size_t i;

    if (!p) {
        return 0;
    }
    tag(&p->body);
    items = arena_alloc(arena, (file->body.nitems + p->body.nitems + 1) * sizeof(Item *));
    if (file->body.nitems) {
        memcpy(items, file->body.items, file->body.nitems * sizeof(Item *));
    }
    for (i = 0; i < p->body.nitems; i++) {
        items[file->body.nitems + i] = p->body.items[i];
    }
    file->body.items = items;
    file->body.nitems += p->body.nitems;
    return 1;
}
