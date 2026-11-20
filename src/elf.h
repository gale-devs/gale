#ifndef GALE_ELF_H
#define GALE_ELF_H

#include <stddef.h>

/* A minimal static x86-64 Linux executable: an ELF header, one program header and
 * the code, loaded as a single read+execute segment. No section headers, no
 * dynamic linking, no libc.
 *
 * `entry_off` is the offset of the entry point inside `code`. Returns a malloc'ed
 * buffer holding the whole file (length in *out_len), or NULL if out of memory. */
unsigned char *elf_build(const unsigned char *code, size_t code_len, size_t entry_off, size_t *out_len);

/* Size of the headers in front of the code. */
#define ELF_HEADERS_SIZE 120

#endif /* GALE_ELF_H */
