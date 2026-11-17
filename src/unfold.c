#include "unfold.h"

#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common.h"
#include "fsutil.h"
#include "parser.h"

struct Unfolder {
    Arena *arena;
    const char *const *roots;
    size_t nroots;
    char **owned; /* malloc'ed buffers freed by unfold_free */
    size_t nowned, capowned;
    UnfoldError *err;
    jmp_buf jb;
};

/* ---- plumbing ---- */

static void oom(void) {
    fprintf(stderr, "gale: out of memory\n");
    exit(1);
}

static NORETURN void fail(Unfolder *u, const char *file, int line, int col, const char *fmt, ...) {
    va_list ap;

    snprintf(u->err->file, sizeof u->err->file, "%s", file ? file : "");
    va_start(ap, fmt);
    vsnprintf(u->err->msg, sizeof u->err->msg, fmt, ap);
    va_end(ap);
    u->err->line = line;
    u->err->col = col;
    longjmp(u->jb, 1);
}

/* Registers a malloc'ed pointer to be freed with the Unfolder. */
static void *own(Unfolder *u, void *p) {
    if (!p) {
        oom();
    }
    if (u->nowned == u->capowned) {
        size_t cap = u->capowned ? u->capowned * 2 : 16;
        char **n = realloc(u->owned, cap * sizeof(char *));
        if (!n) {
            oom();
        }
        u->owned = n;
        u->capowned = cap;
    }
    u->owned[u->nowned++] = p;
    return p;
}

Unfolder *unfold_new(Arena *arena, const char *const *roots, size_t nroots) {
    Unfolder *u = calloc(1, sizeof *u);
    if (!u) {
        oom();
    }
    u->arena = arena;
    u->roots = roots;
    u->nroots = nroots;
    return u;
}

void unfold_free(Unfolder *u) {
    size_t i;
    if (!u) {
        return;
    }
    for (i = 0; i < u->nowned; i++) {
        free(u->owned[i]);
    }
    free(u->owned);
    free(u);
}

/* ---- small AST helpers ---- */

static int str_eq(Str a, Str b) {
    return a.len == b.len && memcmp(a.ptr, b.ptr, a.len) == 0;
}

static int item_name(const Item *it, Str *out) {
    switch (it->kind) {
    case ITEM_FUNC:
        *out = it->u.func.name;
        return 1;
    case ITEM_STRUCT:
        *out = it->u.struct_.name;
        return 1;
    default:
        return 0;
    }
}

static const char *kind_name(const Item *it) {
    return it->kind == ITEM_FUNC ? "func" : "struct";
}

/* The func or struct called `name` in b (unfold items are not members). */
static Item *find_member(const Body *b, Str name) {
    size_t i;
    for (i = 0; i < b->nitems; i++) {
        Str n;
        if (item_name(b->items[i], &n) && str_eq(n, name)) {
            return b->items[i];
        }
    }
    return NULL;
}

static Field *find_field(const Body *b, Str name) {
    size_t i;
    for (i = 0; i < b->nfields; i++) {
        if (str_eq(b->fields[i]->name, name)) {
            return b->fields[i];
        }
    }
    return NULL;
}

static void append_item(Unfolder *u, Body *b, Item *it) {
    Item **n = arena_alloc(u->arena, (b->nitems + 1) * sizeof(Item *));
    if (b->nitems) {
        memcpy(n, b->items, b->nitems * sizeof(Item *));
    }
    n[b->nitems] = it;
    b->items = n;
    b->nitems++;
}

static void append_field(Unfolder *u, Body *b, Field *f) {
    Field **n = arena_alloc(u->arena, (b->nfields + 1) * sizeof(Field *));
    if (b->nfields) {
        memcpy(n, b->fields, b->nfields * sizeof(Field *));
    }
    n[b->nfields] = f;
    b->fields = n;
    b->nfields++;
}

static Item *new_struct_item(Unfolder *u, Str name, int exported, int line, int col) {
    Item *it = arena_alloc(u->arena, sizeof(Item));
    it->kind = ITEM_STRUCT;
    it->exported = exported;
    it->line = line;
    it->col = col;
    it->u.struct_.name = name;
    return it;
}

/* "a.b.c" for the first `count` segments of a path (malloc'ed, owned). */
static char *path_text(Unfolder *u, const Path *p, size_t count) {
    size_t len = 1, i;
    char *s, *w;

    for (i = 0; i < count; i++) {
        len += p->segs[i].len + 1;
    }
    s = own(u, malloc(len));
    w = s;
    for (i = 0; i < count; i++) {
        if (i) {
            *w++ = '.';
        }
        memcpy(w, p->segs[i].ptr, p->segs[i].len);
        w += p->segs[i].len;
    }
    *w = '\0';
    return s;
}

/* ---- merging ---- */

/* Where a merge conflict is reported: the unfold item that caused it. */
typedef struct {
    const char *file;
    int line, col;
    const char *what; /* the unfolded path, for messages */
} Site;

static char *trail_join(Unfolder *u, const char *trail, Str name) {
    size_t tl = strlen(trail);
    char *s = own(u, malloc(tl + name.len + 2));
    size_t w = 0;

    if (tl) {
        memcpy(s, trail, tl);
        w = tl;
        s[w++] = '.';
    }
    memcpy(s + w, name.ptr, name.len);
    s[w + name.len] = '\0';
    return s;
}

static void merge_item(Unfolder *u, Body *dst, Item *it, const char *trail, const Site *site);

static void merge_body(Unfolder *u, Body *dst, Body *src, const char *trail, const Site *site) {
    size_t i;

    for (i = 0; i < src->nfields; i++) {
        Field *f = src->fields[i];
        if (find_field(dst, f->name)) {
            fail(u, site->file, site->line, site->col,
                 "unfolding '%s': field '%s%s%.*s' is already defined", site->what, trail,
                 *trail ? "." : "", (int)f->name.len, f->name.ptr);
        }
        append_field(u, dst, f);
    }
    for (i = 0; i < src->nitems; i++) {
        merge_item(u, dst, src->items[i], trail, site);
    }
}

/* Adds `it` to dst. Structs with the same name are merged; anything else with the
 * same name is a conflict. */
static void merge_item(Unfolder *u, Body *dst, Item *it, const char *trail, const Site *site) {
    Str name;
    Item *ex;

    if (!item_name(it, &name)) {
        return;
    }
    ex = find_member(dst, name);
    if (!ex) {
        append_item(u, dst, it);
        return;
    }
    if (ex->kind == ITEM_STRUCT && it->kind == ITEM_STRUCT) {
        ex->exported = ex->exported || it->exported;
        merge_body(u, &ex->u.struct_.body, &it->u.struct_.body, trail_join(u, trail, name), site);
        return;
    }
    if (ex->kind == it->kind) {
        fail(u, site->file, site->line, site->col, "unfolding '%s': %s '%s' is defined more than once",
             site->what, kind_name(ex), trail_join(u, trail, name));
    }
    fail(u, site->file, site->line, site->col,
         "unfolding '%s': '%s' is defined as both a %s and a %s", site->what,
         trail_join(u, trail, name), kind_name(ex), kind_name(it));
}

/* ---- finding and expanding ---- */

/* <root>/seg0/seg1/.../seg(k-1).gale  (malloc'ed, owned) */
static char *candidate(Unfolder *u, const char *root, const Path *p, size_t k) {
    size_t len = strlen(root) + 1 + 5 + 1, i;
    char *s, *w;

    for (i = 0; i < k; i++) {
        len += p->segs[i].len + 1;
    }
    s = own(u, malloc(len));
    w = s;
    memcpy(w, root, strlen(root));
    w += strlen(root);
    for (i = 0; i < k; i++) {
        *w++ = '/';
        memcpy(w, p->segs[i].ptr, p->segs[i].len);
        w += p->segs[i].len;
    }
    memcpy(w, ".gale", 6);
    return s;
}

static void expand_body(Unfolder *u, Body *body, const char *cur_file);

static void process_unfold(Unfolder *u, Body *dst, const Item *uf, const char *cur_file) {
    const Path *path = &uf->u.unfold;
    char *what = path_text(u, path, path->count);
    char tried[400];
    char *file_path = NULL;
    char *text = NULL;
    size_t r, k = 0;
    Site site;
    ParseError perr;
    File *file;
    Item *leaf;
    size_t i;

    site.file = cur_file;
    site.line = uf->line;
    site.col = uf->col;
    site.what = what;

    /* The first k segments name a file; the rest name something inside it. The
     * shortest prefix that is a file wins. */
    tried[0] = '\0';
    for (r = 0; r < u->nroots && !text; r++) {
        size_t kk;
        for (kk = 1; kk <= path->count && !text; kk++) {
            char *cand = candidate(u, u->roots[r], path, kk);
            text = read_file(cand);
            if (text) {
                file_path = cand;
                k = kk;
            } else {
                size_t used = strlen(tried);
                snprintf(tried + used, sizeof tried - used, "%s%s", used ? ", " : "", cand);
            }
        }
    }
    if (!text) {
        fail(u, cur_file, uf->line, uf->col, "cannot find '%s' (looked for: %s)", what,
             tried[0] ? tried : "no search directories");
    }
    own(u, text);

    file = parse_file(text, u->arena, &perr);
    if (!file) {
        fail(u, file_path, perr.line, perr.col, "%s", perr.msg);
    }
    expand_body(u, &file->body, file_path);

    if (k == path->count) {
        /* the whole file: a struct named after the last segment ... */
        leaf = new_struct_item(u, path->segs[path->count - 1], uf->exported, uf->line, uf->col);
        leaf->u.struct_.body = file->body;
    } else {
        /* part of the file: walk down through structs to the named struct or func */
        Body *cur = &file->body;
        for (i = k; i + 1 < path->count; i++) {
            Item *m = find_member(cur, path->segs[i]);
            if (!m || m->kind != ITEM_STRUCT) {
                fail(u, cur_file, uf->line, uf->col, "cannot find '%.*s' in '%s' (file %s)",
                     (int)path->segs[i].len, path->segs[i].ptr, path_text(u, path, i), file_path);
            }
            cur = &m->u.struct_.body;
        }
        leaf = find_member(cur, path->segs[path->count - 1]);
        if (!leaf) {
            fail(u, cur_file, uf->line, uf->col, "cannot find '%.*s' in '%s' (file %s)",
                 (int)path->segs[path->count - 1].len, path->segs[path->count - 1].ptr,
                 path_text(u, path, path->count - 1), file_path);
        }
    }
    /* ... wrapped in one struct per remaining segment */
    for (i = path->count - 1; i > 0; i--) {
        Item *w = new_struct_item(u, path->segs[i - 1], uf->exported, uf->line, uf->col);
        append_item(u, &w->u.struct_.body, leaf);
        leaf = w;
    }
    merge_item(u, dst, leaf, "", &site);
}

/* Expands the unfold items of a body (and of the structs inside it) in place. */
static void expand_body(Unfolder *u, Body *body, const char *cur_file) {
    Item **orig = body->items;
    size_t n0 = body->nitems;
    size_t i, w;
    Item **out;

    for (i = 0; i < n0; i++) {
        if (orig[i]->kind == ITEM_STRUCT) {
            expand_body(u, &orig[i]->u.struct_.body, cur_file);
        }
    }
    for (i = 0; i < n0; i++) {
        if (orig[i]->kind == ITEM_UNFOLD) {
            process_unfold(u, body, orig[i], cur_file);
        }
    }

    out = arena_alloc(u->arena, (body->nitems + 1) * sizeof(Item *));
    w = 0;
    for (i = 0; i < body->nitems; i++) {
        if (body->items[i]->kind != ITEM_UNFOLD) {
            out[w++] = body->items[i];
        }
    }
    body->items = out;
    body->nitems = w;
}

int unfold_expand(Unfolder *u, File *file, const char *entry_path, UnfoldError *err) {
    u->err = err;
    err->file[0] = '\0';
    err->msg[0] = '\0';
    err->line = 0;
    err->col = 0;
    if (setjmp(u->jb)) {
        return 0;
    }
    expand_body(u, &file->body, entry_path);
    return 1;
}
