#include "pe.h"

#include <stdlib.h>
#include <string.h>

#define IMAGE_BASE 0x140000000ULL
#define SECTION_ALIGN 0x1000UL
#define FILE_ALIGN 0x200UL
#define TEXT_RVA 0x1000UL
#define HEADERS_SIZE 0x200UL

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

static size_t align_up(size_t n, size_t a) {
    return (n + a - 1) / a * a;
}

unsigned char *pe_build(const Asm *as, const Code *code, size_t entry_off, size_t *out_len) {
    size_t nimp = asm_import_count(as), i, j;
    size_t *slots = calloc(nimp + 1, sizeof(size_t));
    size_t *hint_off = calloc(nimp + 1, sizeof(size_t));
    size_t *dll_name_off = calloc(nimp + 1, sizeof(size_t));
    size_t *dll_first_slot = calloc(nimp + 1, sizeof(size_t));
    const char **dlls = calloc(nimp + 1, sizeof(char *));
    size_t ndll = 0, nslots = 0;
    size_t iat_off, iat_size = 0, ilt_off = 0, dir_off = 0, dir_size = 0, names_off, cur, total;
    size_t raw, file_len;
    unsigned char *f = NULL, *sec;
    unsigned char *opt;
    const Import *all;

    if (!slots || !hint_off || !dll_name_off || !dll_first_slot || !dlls) {
        goto out;
    }
    all = nimp ? asm_import_at(as, 0) : NULL;

    iat_off = code->import_base;
    cur = code->len > iat_off ? code->len : iat_off;
    if (nimp) {
        nslots = asm_import_slots(as, slots);
        iat_size = nslots * 8;
        /* distinct DLLs, in order of first appearance (the order asm_import_slots used) */
        for (i = 0; i < nimp; i++) {
            int seen = 0;
            for (j = 0; j < ndll; j++) {
                if (strcmp(dlls[j], all[i].dll) == 0) {
                    seen = 1;
                    break;
                }
            }
            if (!seen) {
                dlls[ndll++] = all[i].dll;
            }
        }
        for (j = 0; j < ndll; j++) {
            size_t first = (size_t)-1;
            for (i = 0; i < nimp; i++) {
                if (strcmp(all[i].dll, dlls[j]) == 0 && slots[i] < first) {
                    first = slots[i];
                }
            }
            dll_first_slot[j] = first;
        }
        ilt_off = iat_off + iat_size;
        dir_off = ilt_off + iat_size;
        dir_size = (ndll + 1) * 20;
        names_off = dir_off + dir_size;
        cur = names_off;
        for (i = 0; i < nimp; i++) { /* hint (2 bytes) + name + NUL, padded to even */
            hint_off[i] = cur;
            cur += align_up(2 + strlen(all[i].name) + 1, 2);
        }
        for (j = 0; j < ndll; j++) {
            dll_name_off[j] = cur;
            cur += strlen(dlls[j]) + 1;
        }
    }
    total = cur;
    raw = align_up(total, FILE_ALIGN);
    file_len = HEADERS_SIZE + raw;
    f = calloc(1, file_len);
    if (!f) {
        goto out;
    }

    /* DOS header: just "MZ" and where the PE header is */
    f[0] = 'M';
    f[1] = 'Z';
    le32(f + 0x3C, 0x40);

    /* PE signature and COFF header */
    memcpy(f + 0x40, "PE\0\0", 4);
    le16(f + 0x44, 0x8664);  /* Machine: x64 */
    le16(f + 0x46, 1);       /* NumberOfSections */
    le16(f + 0x54, 240);     /* SizeOfOptionalHeader */
    le16(f + 0x56, 0x0022);  /* executable image, large address aware */

    /* optional header (PE32+) */
    opt = f + 0x58;
    le16(opt + 0, 0x20B);
    opt[2] = 14;
    le32(opt + 4, (unsigned long)align_up(code->len, FILE_ALIGN));   /* SizeOfCode */
    le32(opt + 16, (unsigned long)(TEXT_RVA + entry_off));           /* AddressOfEntryPoint */
    le32(opt + 20, TEXT_RVA);                                        /* BaseOfCode */
    le64(opt + 24, IMAGE_BASE);
    le32(opt + 32, SECTION_ALIGN);
    le32(opt + 36, FILE_ALIGN);
    le16(opt + 40, 6);                                               /* OS version 6.0 */
    le16(opt + 48, 6);                                               /* subsystem version 6.0 */
    le32(opt + 56, (unsigned long)(TEXT_RVA + align_up(total, SECTION_ALIGN))); /* SizeOfImage */
    le32(opt + 60, HEADERS_SIZE);
    le16(opt + 68, 3);                                               /* console */
    le64(opt + 72, 0x100000);                                        /* stack reserve */
    le64(opt + 80, 0x1000);                                          /* stack commit */
    le64(opt + 88, 0x100000);                                        /* heap reserve */
    le64(opt + 96, 0x1000);                                          /* heap commit */
    le32(opt + 108, 16);                                             /* data directories */
    if (nimp) {
        le32(opt + 112 + 1 * 8, (unsigned long)(TEXT_RVA + dir_off));    /* import directory */
        le32(opt + 112 + 1 * 8 + 4, (unsigned long)dir_size);
        le32(opt + 112 + 12 * 8, (unsigned long)(TEXT_RVA + iat_off));   /* IAT */
        le32(opt + 112 + 12 * 8 + 4, (unsigned long)iat_size);
    }

    /* section header: one section holding everything */
    sec = f + 0x58 + 240;
    memcpy(sec, ".text", 5);
    le32(sec + 8, (unsigned long)total);          /* VirtualSize */
    le32(sec + 12, TEXT_RVA);                     /* VirtualAddress */
    le32(sec + 16, (unsigned long)raw);           /* SizeOfRawData */
    le32(sec + 20, HEADERS_SIZE);                 /* PointerToRawData */
    le32(sec + 36, 0xE0000020UL);                 /* code, execute, read, write */

    /* section contents */
    if (code->len) {
        memcpy(f + HEADERS_SIZE, code->data, code->len);
    }
    if (nimp) {
        unsigned char *base = f + HEADERS_SIZE;
        for (i = 0; i < nimp; i++) {
            unsigned long hn_rva = (unsigned long)(TEXT_RVA + hint_off[i]);
            le64(base + iat_off + 8 * slots[i], hn_rva);  /* IAT: points at the name until bound */
            le64(base + ilt_off + 8 * slots[i], hn_rva);  /* import lookup table */
            memcpy(base + hint_off[i] + 2, all[i].name, strlen(all[i].name));
        }
        for (j = 0; j < ndll; j++) {
            unsigned char *d = base + dir_off + 20 * j;
            le32(d + 0, (unsigned long)(TEXT_RVA + ilt_off + 8 * dll_first_slot[j]));  /* OriginalFirstThunk */
            le32(d + 12, (unsigned long)(TEXT_RVA + dll_name_off[j]));                   /* Name */
            le32(d + 16, (unsigned long)(TEXT_RVA + iat_off + 8 * dll_first_slot[j]));  /* FirstThunk */
            memcpy(base + dll_name_off[j], dlls[j], strlen(dlls[j]));
        }
    }
    *out_len = file_len;

out:
    free(slots);
    free(hint_off);
    free(dll_name_off);
    free(dll_first_slot);
    free((void *)dlls);
    return f;
}
