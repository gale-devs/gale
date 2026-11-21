#ifndef GALE_PE_H
#define GALE_PE_H

#include <stddef.h>

#include "asm.h"
#include "encode.h"

/* A minimal Windows x64 console executable (PE32+): the DOS header and PE headers,
 * then a single section (read, write, execute) that holds the code, the import
 * address table, and the import directory with the names it needs. No DOS stub, no
 * relocations (the code is position-independent), no resources.
 *
 * `entry_off` is the offset of the entry point inside the code. The imports are the
 * ones declared in `as`. Returns a malloc'ed buffer with the whole file (length in
 * *out_len), or NULL if out of memory. */
unsigned char *pe_build(const Asm *as, const Code *code, size_t entry_off, size_t *out_len);

#endif /* GALE_PE_H */
