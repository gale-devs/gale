#include "elf.h"

#include <stdlib.h>
#include <string.h>

#define BASE_ADDR 0x400000UL

static void le16(unsigned char *p, unsigned v) {
    p[0] = (unsigned char)(v & 0xFF);
    p[1] = (unsigned char)((v >> 8) & 0xFF);
}

static void le32(unsigned char *p, unsigned long v) {
    int i;
    for (i = 0; i < 4; i++) {
        p[i] = (unsigned char)((v >> (8 * i)) & 0xFF);
    }
}

static void le64(unsigned char *p, unsigned long long v) {
    int i;
    for (i = 0; i < 8; i++) {
        p[i] = (unsigned char)((v >> (8 * i)) & 0xFF);
    }
}

unsigned char *elf_build(const unsigned char *code, size_t code_len, size_t entry_off, size_t *out_len) {
    size_t total = ELF_HEADERS_SIZE + code_len;
    unsigned char *f = calloc(1, total);
    unsigned char *eh, *ph;

    if (!f) {
        return NULL;
    }
    eh = f;
    ph = f + 64;

    /* ELF header */
    eh[0] = 0x7F;
    eh[1] = 'E';
    eh[2] = 'L';
    eh[3] = 'F';
    eh[4] = 2; /* 64-bit */
    eh[5] = 1; /* little endian */
    eh[6] = 1; /* version */
    le16(eh + 16, 2);    /* e_type: executable */
    le16(eh + 18, 0x3E); /* e_machine: x86-64 */
    le32(eh + 20, 1);    /* e_version */
    le64(eh + 24, BASE_ADDR + ELF_HEADERS_SIZE + entry_off); /* e_entry */
    le64(eh + 32, 64);   /* e_phoff */
    le64(eh + 40, 0);    /* e_shoff: no sections */
    le32(eh + 48, 0);    /* e_flags */
    le16(eh + 52, 64);   /* e_ehsize */
    le16(eh + 54, 56);   /* e_phentsize */
    le16(eh + 56, 1);    /* e_phnum */
    le16(eh + 58, 0);    /* e_shentsize */
    le16(eh + 60, 0);    /* e_shnum */
    le16(eh + 62, 0);    /* e_shstrndx */

    /* program header: one PT_LOAD covering the whole file, read + execute */
    le32(ph + 0, 1);              /* p_type: PT_LOAD */
    le32(ph + 4, 5);              /* p_flags: R + X */
    le64(ph + 8, 0);              /* p_offset */
    le64(ph + 16, BASE_ADDR);     /* p_vaddr */
    le64(ph + 24, BASE_ADDR);     /* p_paddr */
    le64(ph + 32, total);         /* p_filesz */
    le64(ph + 40, total);         /* p_memsz */
    le64(ph + 48, 0x1000);        /* p_align */

    if (code_len) {
        memcpy(f + ELF_HEADERS_SIZE, code, code_len);
    }
    *out_len = total;
    return f;
}
