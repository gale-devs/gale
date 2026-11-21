#ifndef GALE_ENCODE_H
#define GALE_ENCODE_H

#include <stddef.h>

#include "arena.h"
#include "asm.h"

typedef struct {
    unsigned char *data;
    size_t len;
    /* Offset (from the start of the code) of the import address table, which the
     * executable writer places right after the code. Calls to imports are encoded
     * relative to it. */
    size_t import_base;
} Code;

/* Encodes a whole program to x86-64 machine code (allocated in the arena).
 *
 * Jumps use the short form when the target is within reach, so the code is as small
 * as this simple scheme can make it. `label_off` must have room for
 * asm_label_count(a) entries; each gets the code offset of its label.
 *
 * Returns 1 on success. On failure (an operand form the encoder does not support, a
 * label that is used but never defined) returns 0 and writes a message to err. */
int asm_encode(const Asm *a, Arena *arena, Code *out, size_t *label_off, char *err, size_t errcap);

/* Encodes one instruction that does not refer to a label (not a jump, call or
 * label). `out` must hold 15 bytes. Returns the length, or 0 on failure. */
size_t inst_encode(const Inst *in, unsigned char *out);

#endif /* GALE_ENCODE_H */
