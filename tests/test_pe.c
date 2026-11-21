#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "arena.h"
#include "asm.h"
#include "encode.h"
#include "pe.h"

static int failures = 0;

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            failures++;                                                          \
        }                                                                        \
    } while (0)

static unsigned long long rd(const unsigned char *p, int n) {
    unsigned long long v = 0;
    int i;
    for (i = n - 1; i >= 0; i--) {
        v = (v << 8) | p[i];
    }
    return v;
}

/* The file offset of a relative virtual address, given the one section's headers. */
static const unsigned char *at_rva(const unsigned char *f, size_t len, unsigned long rva) {
    unsigned long va = (unsigned long)rd(f + 0x148 + 12, 4);
    unsigned long raw = (unsigned long)rd(f + 0x148 + 20, 4);
    unsigned long size = (unsigned long)rd(f + 0x148 + 16, 4);

    if (rva < va || rva - va >= size || raw + (rva - va) >= len) {
        return NULL;
    }
    return f + raw + (rva - va);
}

static void test_headers(void) {
    Arena ar;
    Asm a;
    Code code;
    size_t labels[2];
    char err[200];
    size_t len = 0;
    unsigned char *f;
    int start, ex;
    unsigned long dir_rva, iat_rva;
    const unsigned char *d, *ilt, *iat, *name;

    arena_init(&ar);
    asm_init(&a, &ar);
    start = asm_new_label(&a, "_start");
    ex = asm_import(&a, "kernel32.dll", "ExitProcess");
    asm_op2(&a, X_MOV, 8, opd_reg(RCX), opd_imm(7));
    asm_label(&a, start);
    asm_op1(&a, X_CALL, opd_import(ex));
    CHECK(asm_encode(&a, &ar, &code, labels, err, sizeof err));
    f = pe_build(&a, &code, labels[start], &len);
    CHECK(f != NULL);
    if (!f) {
        arena_free(&ar);
        return;
    }

    CHECK(f[0] == 'M' && f[1] == 'Z');
    CHECK(rd(f + 0x3C, 4) == 0x40);                        /* e_lfanew */
    CHECK(memcmp(f + 0x40, "PE\0\0", 4) == 0);
    CHECK(rd(f + 0x44, 2) == 0x8664);                      /* x64 */
    CHECK(rd(f + 0x46, 2) == 1);                           /* one section */
    CHECK(rd(f + 0x54, 2) == 240);                         /* optional header size */
    CHECK(rd(f + 0x56, 2) == 0x22);
    CHECK(rd(f + 0x58, 2) == 0x20B);                       /* PE32+ */
    CHECK(rd(f + 0x58 + 16, 4) == 0x1000 + labels[start]); /* entry point */
    CHECK(rd(f + 0x58 + 24, 8) == 0x140000000ULL);
    CHECK(rd(f + 0x58 + 32, 4) == 0x1000);                 /* section alignment */
    CHECK(rd(f + 0x58 + 36, 4) == 0x200);                  /* file alignment */
    CHECK(rd(f + 0x58 + 60, 4) == 0x200);                  /* size of headers */
    CHECK(rd(f + 0x58 + 68, 2) == 3);                      /* console subsystem */
    CHECK(rd(f + 0x58 + 108, 4) == 16);
    CHECK(len % 0x200 == 0 && len >= 0x400);
    CHECK(memcmp(f + 0x148, ".text", 6) == 0);
    CHECK(rd(f + 0x148 + 12, 4) == 0x1000);
    CHECK(rd(f + 0x148 + 20, 4) == 0x200);
    CHECK(rd(f + 0x148 + 36, 4) == 0xE0000020UL);
    CHECK(rd(f + 0x58 + 56, 4) == 0x2000);                 /* size of image */
    /* the code is where the section starts */
    CHECK(memcmp(f + 0x200, code.data, code.len) == 0);

    /* import directory: one descriptor for kernel32.dll, then a zero one */
    dir_rva = (unsigned long)rd(f + 0x58 + 112 + 8, 4);
    iat_rva = (unsigned long)rd(f + 0x58 + 112 + 12 * 8, 4);
    CHECK(rd(f + 0x58 + 112 + 8 + 4, 4) == 40);
    CHECK(rd(f + 0x58 + 112 + 12 * 8 + 4, 4) == 16);      /* one slot and its terminator */
    CHECK(iat_rva == 0x1000 + code.import_base);
    d = at_rva(f, len, dir_rva);
    CHECK(d != NULL);
    if (d) {
        name = at_rva(f, len, (unsigned long)rd(d + 12, 4));
        CHECK(name && strcmp((const char *)name, "kernel32.dll") == 0);
        CHECK(rd(d + 16, 4) == iat_rva);                   /* FirstThunk is the IAT */
        ilt = at_rva(f, len, (unsigned long)rd(d, 4));
        iat = at_rva(f, len, iat_rva);
        CHECK(ilt && iat && rd(ilt, 8) == rd(iat, 8) && rd(ilt, 8) != 0 && rd(ilt + 8, 8) == 0);
        name = at_rva(f, len, (unsigned long)rd(ilt, 8));
        CHECK(name && rd(name, 2) == 0 && strcmp((const char *)name + 2, "ExitProcess") == 0);
        CHECK(rd(d + 20, 4) == 0 && rd(d + 20 + 12, 4) == 0); /* the terminating descriptor is empty */
    }
    free(f);
    arena_free(&ar);
}

/* Two DLLs, three imports: check the IAT layout and that every call reaches the slot
 * of the function it names. */
static void test_two_dlls(void) {
    Arena ar;
    Asm a;
    Code code;
    size_t labels[2];
    char err[200];
    size_t len = 0, i;
    unsigned char *f;
    int start, i1, i2, i3;
    const char *want[3] = {"ExitProcess", "MessageBoxA", "GetStdHandle"};
    unsigned long dir_rva;
    const unsigned char *d;

    arena_init(&ar);
    asm_init(&a, &ar);
    start = asm_new_label(&a, "_start");
    i1 = asm_import(&a, "kernel32.dll", want[0]);
    i2 = asm_import(&a, "user32.dll", want[1]);
    i3 = asm_import(&a, "kernel32.dll", want[2]);
    asm_label(&a, start);
    asm_op1(&a, X_CALL, opd_import(i1));
    asm_op1(&a, X_CALL, opd_import(i2));
    asm_op1(&a, X_CALL, opd_import(i3));
    asm_op0(&a, X_RET);
    CHECK(asm_encode(&a, &ar, &code, labels, err, sizeof err));
    f = pe_build(&a, &code, labels[start], &len);
    CHECK(f != NULL);
    if (!f) {
        arena_free(&ar);
        return;
    }

    /* every call goes through a slot that holds the address of the right name */
    for (i = 0; i < 3; i++) {
        unsigned long insn = (unsigned long)(i * 6);
        unsigned long disp = (unsigned long)rd(f + 0x200 + insn + 2, 4);
        unsigned long target = 0x1000 + (unsigned long)insn + 6 + disp;
        const unsigned char *slot = at_rva(f, len, target);
        const unsigned char *hn = slot ? at_rva(f, len, (unsigned long)rd(slot, 8)) : NULL;
        const char *expect = i == 0 ? want[0] : i == 1 ? want[1] : want[2];
        CHECK(f[0x200 + insn] == 0xFF && f[0x200 + insn + 1] == 0x15);
        CHECK(hn && strcmp((const char *)hn + 2, expect) == 0);
    }

    /* two descriptors, each with its own thunk table, then an empty one */
    dir_rva = (unsigned long)rd(f + 0x58 + 112 + 8, 4);
    CHECK(rd(f + 0x58 + 112 + 8 + 4, 4) == 60);
    CHECK(rd(f + 0x58 + 112 + 12 * 8 + 4, 4) == 40);      /* 5 slots */
    d = at_rva(f, len, dir_rva);
    CHECK(d != NULL);
    if (d) {
        const unsigned char *n0 = at_rva(f, len, (unsigned long)rd(d + 12, 4));
        const unsigned char *n1 = at_rva(f, len, (unsigned long)rd(d + 20 + 12, 4));
        CHECK(n0 && strcmp((const char *)n0, "kernel32.dll") == 0);
        CHECK(n1 && strcmp((const char *)n1, "user32.dll") == 0);
        CHECK(rd(d + 20 + 16, 4) == rd(d + 16, 4) + 24);   /* user32's table starts at slot 3 */
        CHECK(rd(d + 40 + 12, 4) == 0 && rd(d + 40 + 16, 4) == 0);
    }
    free(f);
    arena_free(&ar);
}

static void test_no_imports(void) {
    Arena ar;
    Asm a;
    Code code;
    size_t labels[2];
    char err[200];
    size_t len = 0;
    unsigned char *f;
    int start;

    arena_init(&ar);
    asm_init(&a, &ar);
    start = asm_new_label(&a, "_start");
    asm_label(&a, start);
    asm_op0(&a, X_RET);
    CHECK(asm_encode(&a, &ar, &code, labels, err, sizeof err));
    f = pe_build(&a, &code, labels[start], &len);
    CHECK(f != NULL && len == 0x400);
    if (f) {
        CHECK(rd(f + 0x58 + 112 + 8 + 4, 4) == 0);        /* no import directory */
        CHECK(f[0x200] == 0xC3);
    }
    free(f);
    arena_free(&ar);
}

int main(void) {
    test_headers();
    test_two_dlls();
    test_no_imports();
    if (failures) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    printf("pe: all tests passed\n");
    return 0;
}
