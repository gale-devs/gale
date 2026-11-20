#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "arena.h"
#include "asm.h"
#include "encode.h"

/* The cases come from tools/gen_encode_cases.py: each is Intel-syntax text, the bytes
 * GNU as produces for it, and the Inst that should print as that text and encode to
 * those bytes. */

static int failures = 0;
static int checked = 0;

#define REG(r) opd_reg(r)
#define IMM(v) opd_imm(v)
#define MEM(r, d) opd_mem(r, d)

typedef struct {
    const char *name;
    const char *hex;
    void (*build)(Asm *a);
} SeqCase;

static void to_hex(const unsigned char *data, size_t len, char *out) {
    size_t i;
    out[0] = '\0';
    for (i = 0; i < len; i++) {
        sprintf(out + strlen(out), "%s%02x", i ? " " : "", data[i]);
    }
}

static void check_case(const char *text, const char *hex, Inst in) {
    Arena ar;
    Asm a;
    unsigned char buf[16];
    char got[64];
    char *printed;
    size_t n;

    arena_init(&ar);
    asm_init(&a, &ar);
    checked++;

    printed = asm_format_inst(&a, &in);
    if (strcmp(printed, text) != 0) {
        fprintf(stderr, "printer: got \"%s\", want \"%s\"\n", printed, text);
        failures++;
    }
    free(printed);

    n = inst_encode(&in, buf);
    if (n == 0) {
        fprintf(stderr, "cannot encode \"%s\"\n", text);
        failures++;
    } else {
        to_hex(buf, n, got);
        if (strcmp(got, hex) != 0) {
            fprintf(stderr, "encoding of \"%s\":\n  got  %s\n  want %s\n", text, got, hex);
            failures++;
        }
    }
    arena_free(&ar);
}

#define CASE(text, hex, inst) check_case(text, hex, inst)

static void run_inst_cases(void) {
#include "encode_cases.inc"
}

#include "encode_seqs.inc"

static void run_seq_cases(void) {
    size_t i;

    for (i = 0; i < sizeof SEQS / sizeof SEQS[0]; i++) {
        Arena ar;
        Asm a;
        Code code;
        size_t *labels;
        char err[200];
        char *got;

        arena_init(&ar);
        asm_init(&a, &ar);
        SEQS[i].build(&a);
        labels = arena_alloc(&ar, (asm_label_count(&a) + 1) * sizeof(size_t));
        checked++;
        if (!asm_encode(&a, &ar, &code, labels, err, sizeof err)) {
            fprintf(stderr, "sequence \"%s\": %s\n", SEQS[i].name, err);
            failures++;
        } else {
            got = malloc(code.len * 3 + 1);
            to_hex(code.data, code.len, got);
            if (strcmp(got, SEQS[i].hex) != 0) {
                fprintf(stderr, "sequence \"%s\":\n  got  %s\n  want %s\n", SEQS[i].name, got, SEQS[i].hex);
                failures++;
            }
            free(got);
        }
        arena_free(&ar);
    }
}

static void test_errors(void) {
    Arena ar;
    Asm a;
    Code code;
    size_t labels[4];
    char err[200];
    int l;

    /* a label that is used but never defined */
    arena_init(&ar);
    asm_init(&a, &ar);
    l = asm_new_label(&a, NULL);
    asm_op1(&a, X_JMP, opd_label(l));
    checked++;
    if (asm_encode(&a, &ar, &code, labels, err, sizeof err) || !strstr(err, "not defined")) {
        fprintf(stderr, "expected an 'undefined label' error, got: %s\n", err);
        failures++;
    }
    arena_free(&ar);

    /* an operand form that does not exist */
    arena_init(&ar);
    asm_init(&a, &ar);
    asm_op2(&a, X_MOV, 8, opd_imm(1), opd_reg(RAX));
    checked++;
    if (asm_encode(&a, &ar, &code, labels, err, sizeof err) || !strstr(err, "cannot encode")) {
        fprintf(stderr, "expected a 'cannot encode' error, got: %s\n", err);
        failures++;
    }
    arena_free(&ar);
}

static void test_labels_and_names(void) {
    Arena ar;
    Asm a;
    Code code;
    size_t labels[4];
    char err[200];
    int f, g;
    char *text;

    arena_init(&ar);
    asm_init(&a, &ar);
    f = asm_new_label(&a, "main");
    g = asm_new_label(&a, NULL);
    asm_label(&a, f);
    asm_op1(&a, X_PUSH, opd_reg(RBP));
    asm_op2(&a, X_MOV, 8, opd_reg(RBP), opd_reg(RSP));
    asm_op1(&a, X_CALL, opd_label(g));
    asm_op0(&a, X_RET);
    asm_label(&a, g);
    asm_op0(&a, X_RET);
    checked++;
    if (!asm_encode(&a, &ar, &code, labels, err, sizeof err)) {
        fprintf(stderr, "encode failed: %s\n", err);
        failures++;
    } else if (labels[f] != 0 || labels[g] != 1 + 3 + 5 + 1) {
        fprintf(stderr, "wrong label offsets: %lu %lu\n", (unsigned long)labels[f], (unsigned long)labels[g]);
        failures++;
    }
    text = asm_format(&a);
    if (strcmp(text, "main:\n    push rbp\n    mov rbp, rsp\n    call .L1\n    ret\n.L1:\n    ret\n") != 0) {
        fprintf(stderr, "wrong program text:\n%s", text);
        failures++;
    }
    free(text);
    arena_free(&ar);
}

int main(void) {
    run_inst_cases();
    run_seq_cases();
    test_errors();
    test_labels_and_names();

    if (failures) {
        fprintf(stderr, "%d of %d check(s) failed\n", failures, checked);
        return 1;
    }
    printf("encode: all %d checks passed\n", checked);
    return 0;
}
