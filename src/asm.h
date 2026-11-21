#ifndef GALE_ASM_H
#define GALE_ASM_H

#include <stddef.h>

#include "arena.h"

/* Gale's own assembly: a list of x86-64 instructions in memory. It can be printed
 * as text (Intel syntax, so GNU as can read it too) and encoded straight to machine
 * code (encode.c), without an external assembler or linker. */

typedef enum {
    RAX, RCX, RDX, RBX, RSP, RBP, RSI, RDI,
    R8, R9, R10, R11, R12, R13, R14, R15
} Reg;

typedef enum {
    OPD_NONE,
    OPD_REG,
    OPD_IMM,
    OPD_MEM,   /* [reg + disp] */
    OPD_LABEL,
    OPD_IMPORT /* an imported function (X_CALL only): call qword ptr [rip + its import slot] */
} OpdKind;

typedef struct {
    OpdKind kind;
    Reg reg;         /* REG: the register; MEM: the base register */
    long long imm;   /* IMM */
    long disp;       /* MEM */
    int label;       /* LABEL: the label; IMPORT: the import index */
} Opd;

typedef enum {
    X_MOV,     /* mov  dst, src        (size = operand width: 8, 4, 2 or 1) */
    X_MOVZX,   /* movzx r32, r/m8|16   (size = source width; zero-extends to 64 bits) */
    X_MOVSX,   /* movsx r64, r/m8|16   (size = source width) */
    X_MOVSXD,  /* movsxd r64, r/m32 */
    X_LEA,
    X_ADD, X_OR, X_AND, X_SUB, X_XOR, X_CMP,   /* 64-bit */
    X_TEST,    /* test r64, r64 */
    X_IMUL,    /* imul r64, r/m64 */
    X_IDIV, X_DIV, X_NEG, X_NOT,   /* one 64-bit operand */
    X_CQO,
    X_SHL, X_SHR, X_SAR,  /* r/m64, cl  or  r/m64, imm8 */
    X_PUSH, X_POP,
    X_CALL,    /* call label, or call an import */
    X_RET,
    X_JMP,     /* jmp label */
    X_JCC,     /* j<cc> label */
    X_SETCC,   /* set<cc> r8 */
    X_SYSCALL,
    X_REP_MOVSB,
    X_NOP,
    X_LABEL    /* defines label a at this point */
} XOp;

/* Condition codes (the x86 encoding values). */
typedef enum {
    CC_B = 2, CC_AE = 3, CC_E = 4, CC_NE = 5, CC_BE = 6, CC_A = 7,
    CC_L = 12, CC_GE = 13, CC_LE = 14, CC_G = 15
} Cond;

typedef struct {
    XOp op;
    Cond cc;   /* JCC, SETCC */
    int size;  /* see XOp */
    Opd a, b;
} Inst;

typedef struct {
    const char *dll;   /* e.g. "kernel32.dll" */
    const char *name;  /* e.g. "ExitProcess" */
} Import;

typedef struct {
    Arena *arena;
    Vec insts;         /* Inst */
    Vec label_names;   /* const char*: NULL for anonymous labels */
    Vec imports;       /* Import */
} Asm;

void asm_init(Asm *a, Arena *arena);

/* A new label. A name (e.g. a function) is used when printing; labels without a
 * name print as .L<number>. */
int asm_new_label(Asm *a, const char *name);
size_t asm_label_count(const Asm *a);
const char *asm_label_name(const Asm *a, int label);

/* Declares a function imported from a DLL (Windows); returns its index. Declaring the
 * same function twice returns the same index. */
int asm_import(Asm *a, const char *dll, const char *name);
size_t asm_import_count(const Asm *a);
const Import *asm_import_at(const Asm *a, size_t i);

/* Where each import's address lives in the import address table (IAT), in 8-byte
 * slots: the imports of one DLL are consecutive and followed by a zero slot. Fills
 * slots[i] for every import (an array of asm_import_count entries) and returns the
 * total number of slots. */
size_t asm_import_slots(const Asm *a, size_t *slots);

void asm_emit(Asm *a, Inst in);
size_t asm_count(const Asm *a);
/* Changes the immediate (second operand) of an instruction already emitted, e.g. the
 * frame size in a function prologue once the body has been generated. */
void asm_set_imm(Asm *a, size_t index, long long value);
/* Removes an instruction already emitted. Labels are not positions, so they stay valid. */
void asm_remove(Asm *a, size_t index);
const Inst *asm_at(const Asm *a, size_t i);

/* Operand constructors. */
Opd opd_reg(Reg r);
Opd opd_imm(long long v);
Opd opd_mem(Reg base, long disp);
Opd opd_label(int label);
Opd opd_import(int import);

/* Instruction constructors. */
Inst inst0(XOp op);
Inst inst1(XOp op, Opd a);
Inst inst2(XOp op, int size, Opd a, Opd b);
Inst inst_cc(XOp op, Cond cc, Opd a);

/* Convenience emitters. */
void asm_op0(Asm *a, XOp op);
void asm_op1(Asm *a, XOp op, Opd x);
void asm_op2(Asm *a, XOp op, int size, Opd x, Opd y);
void asm_jcc(Asm *a, Cond cc, int label);
void asm_setcc(Asm *a, Cond cc, Reg r);
void asm_label(Asm *a, int label);

/* Text form of one instruction or of the whole program (malloc'ed, caller frees). */
char *asm_format_inst(const Asm *a, const Inst *in);
char *asm_format(const Asm *a);

#endif /* GALE_ASM_H */
