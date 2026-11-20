#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "elf.h"

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

static void test_layout(void) {
    unsigned char code[] = {0x90, 0x90, 0xC3};
    size_t len = 0;
    unsigned char *f = elf_build(code, sizeof code, 1, &len);

    CHECK(f != NULL);
    CHECK(len == ELF_HEADERS_SIZE + 3);
    CHECK(memcmp(f, "\x7f" "ELF", 4) == 0);
    CHECK(f[4] == 2 && f[5] == 1 && f[6] == 1);          /* 64-bit, little endian, v1 */
    CHECK(rd(f + 16, 2) == 2);                            /* executable */
    CHECK(rd(f + 18, 2) == 0x3E);                         /* x86-64 */
    CHECK(rd(f + 24, 8) == 0x400000 + 120 + 1);           /* entry = code start + 1 */
    CHECK(rd(f + 32, 8) == 64);                           /* program header offset */
    CHECK(rd(f + 40, 8) == 0);                            /* no section headers */
    CHECK(rd(f + 52, 2) == 64 && rd(f + 54, 2) == 56);
    CHECK(rd(f + 56, 2) == 1 && rd(f + 60, 2) == 0);      /* one segment, no sections */
    /* program header */
    CHECK(rd(f + 64, 4) == 1);                            /* PT_LOAD */
    CHECK(rd(f + 68, 4) == 5);                            /* R + X */
    CHECK(rd(f + 72, 8) == 0);
    CHECK(rd(f + 80, 8) == 0x400000);
    CHECK(rd(f + 96, 8) == len && rd(f + 104, 8) == len);
    CHECK(rd(f + 112, 8) == 0x1000);
    /* the code follows the headers */
    CHECK(memcmp(f + ELF_HEADERS_SIZE, code, sizeof code) == 0);
    free(f);
}

static void test_empty_code(void) {
    size_t len = 0;
    unsigned char *f = elf_build(NULL, 0, 0, &len);
    CHECK(f != NULL && len == ELF_HEADERS_SIZE);
    free(f);
}

int main(void) {
    test_layout();
    test_empty_code();
    if (failures) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    printf("elf: all tests passed\n");
    return 0;
}
