#include "encode.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    unsigned char b[16];
    int n;
} Enc;

static void put(Enc *e, int byte) {
    e->b[e->n++] = (unsigned char)(byte & 0xFF);
}

static void put16(Enc *e, long v) {
    put(e, (int)(v & 0xFF));
    put(e, (int)((v >> 8) & 0xFF));
}

static void put32(Enc *e, long v) {
    int i;
    for (i = 0; i < 4; i++) {
        put(e, (int)((v >> (8 * i)) & 0xFF));
    }
}

static void put64(Enc *e, long long v) {
    int i;
    for (i = 0; i < 8; i++) {
        put(e, (int)((v >> (8 * i)) & 0xFF));
    }
}

static int fits8(long long v) {
    return v >= -128 && v <= 127;
}

static int fits32(long long v) {
    return v >= -2147483648LL && v <= 2147483647LL;
}

static void modrm_rr(Enc *e, int reg, int rm) {
    put(e, 0xC0 | ((reg & 7) << 3) | (rm & 7));
}

/* ModRM (+ SIB, displacement) for [base + disp]. */
static void modrm_mem(Enc *e, int reg, Reg base, long disp) {
    int b = base & 7;
    int mod;

    if (disp == 0 && b != 5) {
        mod = 0; /* rbp/r13 as a base always need a displacement */
    } else if (disp >= -128 && disp <= 127) {
        mod = 1;
    } else {
        mod = 2;
    }
    put(e, (mod << 6) | ((reg & 7) << 3) | (b == 4 ? 4 : b));
    if (b == 4) {
        put(e, 0x24); /* SIB: no index, base = rsp/r12 */
    }
    if (mod == 1) {
        put(e, (int)(disp & 0xFF));
    } else if (mod == 2) {
        put32(e, disp);
    }
}

/* [66] [REX] opcode(s) ModRM... for an r/m operand (register or memory) and a
 * register or opcode-extension field. */
static void emit_rm(Enc *e, int p66, int w, int op0, int op1, int regfield, const Opd *rm, int force_rex) {
    int rex = 0x40 | (w ? 8 : 0) | ((regfield & 8) ? 4 : 0) | ((rm->reg & 8) ? 1 : 0);

    if (p66) {
        put(e, 0x66);
    }
    if (rex != 0x40 || force_rex) {
        put(e, rex);
    }
    put(e, op0);
    if (op1 >= 0) {
        put(e, op1);
    }
    if (rm->kind == OPD_REG) {
        modrm_rr(e, regfield, rm->reg);
    } else {
        modrm_mem(e, regfield, rm->reg, rm->disp);
    }
}

/* Byte registers spl, bpl, sil, dil (and r8b..) need a REX prefix. */
static int byte_rex(const Opd *o) {
    return o->kind == OPD_REG && o->reg >= RSP && o->reg <= RDI;
}

static int is_rm(const Opd *o) {
    return o->kind == OPD_REG || o->kind == OPD_MEM;
}

static int alu_ext(XOp op) {
    switch (op) {
    case X_ADD: return 0;
    case X_OR: return 1;
    case X_AND: return 4;
    case X_SUB: return 5;
    case X_XOR: return 6;
    default: return 7; /* X_CMP */
    }
}

static int enc_mov(Enc *e, const Inst *in) {
    const Opd *a = &in->a, *b = &in->b;
    int size = in->size ? in->size : 8;

    if (a->kind == OPD_REG && b->kind == OPD_REG && (size == 8 || size == 4)) {
        emit_rm(e, 0, size == 8, 0x89, -1, b->reg, a, 0);
        return 1;
    }
    if (a->kind == OPD_REG && b->kind == OPD_IMM && size == 8) {
        long long v = b->imm;
        int r = a->reg;
        if (v >= 0 && v <= 0xFFFFFFFFLL) { /* mov r32, imm32 zero-extends */
            if (r >= 8) {
                put(e, 0x41);
            }
            put(e, 0xB8 + (r & 7));
            put32(e, (long)v);
        } else if (fits32(v)) { /* mov r64, simm32 */
            emit_rm(e, 0, 1, 0xC7, -1, 0, a, 0);
            put32(e, (long)v);
        } else {
            put(e, 0x48 | (r >= 8 ? 1 : 0));
            put(e, 0xB8 + (r & 7));
            put64(e, v);
        }
        return 1;
    }
    if (a->kind == OPD_REG && b->kind == OPD_MEM && (size == 8 || size == 4)) {
        emit_rm(e, 0, size == 8, 0x8B, -1, a->reg, b, 0);
        return 1;
    }
    if (a->kind == OPD_MEM && b->kind == OPD_REG) {
        switch (size) {
        case 8: emit_rm(e, 0, 1, 0x89, -1, b->reg, a, 0); return 1;
        case 4: emit_rm(e, 0, 0, 0x89, -1, b->reg, a, 0); return 1;
        case 2: emit_rm(e, 1, 0, 0x89, -1, b->reg, a, 0); return 1;
        case 1: emit_rm(e, 0, 0, 0x88, -1, b->reg, a, byte_rex(b)); return 1;
        default: return 0;
        }
    }
    if (a->kind == OPD_MEM && b->kind == OPD_IMM) {
        switch (size) {
        case 8:
            if (!fits32(b->imm)) {
                return 0;
            }
            emit_rm(e, 0, 1, 0xC7, -1, 0, a, 0);
            put32(e, (long)b->imm);
            return 1;
        case 4:
            emit_rm(e, 0, 0, 0xC7, -1, 0, a, 0);
            put32(e, (long)b->imm);
            return 1;
        case 2:
            emit_rm(e, 1, 0, 0xC7, -1, 0, a, 0);
            put16(e, (long)b->imm);
            return 1;
        case 1:
            emit_rm(e, 0, 0, 0xC6, -1, 0, a, 0);
            put(e, (int)(b->imm & 0xFF));
            return 1;
        default:
            return 0;
        }
    }
    return 0;
}

static int enc_alu(Enc *e, const Inst *in) {
    const Opd *a = &in->a, *b = &in->b;
    int ext = alu_ext(in->op);

    if (a->kind == OPD_REG && b->kind == OPD_REG) {
        emit_rm(e, 0, 1, 0x01 + ext * 8, -1, b->reg, a, 0);
        return 1;
    }
    if (a->kind == OPD_REG && b->kind == OPD_MEM) {
        emit_rm(e, 0, 1, 0x03 + ext * 8, -1, a->reg, b, 0);
        return 1;
    }
    if (a->kind == OPD_MEM && b->kind == OPD_REG) {
        emit_rm(e, 0, 1, 0x01 + ext * 8, -1, b->reg, a, 0);
        return 1;
    }
    if (is_rm(a) && b->kind == OPD_IMM && fits32(b->imm)) {
        if (a->kind == OPD_REG && a->reg == RAX && !fits8(b->imm)) {
            /* the accumulator has a shorter form: op rax, imm32 */
            put(e, 0x48);
            put(e, 0x05 + ext * 8);
            put32(e, (long)b->imm);
            return 1;
        }
        if (fits8(b->imm)) {
            emit_rm(e, 0, 1, 0x83, -1, ext, a, 0);
            put(e, (int)(b->imm & 0xFF));
        } else {
            emit_rm(e, 0, 1, 0x81, -1, ext, a, 0);
            put32(e, (long)b->imm);
        }
        return 1;
    }
    return 0;
}

/* Encodes one instruction. For JMP and JCC, `short_jump` selects the 8-bit form and
 * `rel` is the displacement from the end of the instruction. */
static int enc_inst(Enc *e, const Inst *in, int short_jump, long rel) {
    const Opd *a = &in->a, *b = &in->b;
    int size = in->size ? in->size : 8;

    e->n = 0;
    switch (in->op) {
    case X_MOV:
        return enc_mov(e, in);
    case X_MOVZX:
        if (a->kind != OPD_REG || !is_rm(b) || (size != 1 && size != 2)) {
            return 0;
        }
        emit_rm(e, 0, 0, 0x0F, size == 1 ? 0xB6 : 0xB7, a->reg, b, size == 1 && byte_rex(b));
        return 1;
    case X_MOVSX:
        if (a->kind != OPD_REG || !is_rm(b) || (size != 1 && size != 2)) {
            return 0;
        }
        emit_rm(e, 0, 1, 0x0F, size == 1 ? 0xBE : 0xBF, a->reg, b, 0);
        return 1;
    case X_MOVSXD:
        if (a->kind != OPD_REG || !is_rm(b)) {
            return 0;
        }
        emit_rm(e, 0, 1, 0x63, -1, a->reg, b, 0);
        return 1;
    case X_LEA:
        if (a->kind != OPD_REG || b->kind != OPD_MEM) {
            return 0;
        }
        emit_rm(e, 0, 1, 0x8D, -1, a->reg, b, 0);
        return 1;
    case X_ADD:
    case X_OR:
    case X_AND:
    case X_SUB:
    case X_XOR:
    case X_CMP:
        return enc_alu(e, in);
    case X_TEST:
        if (a->kind != OPD_REG || b->kind != OPD_REG) {
            return 0;
        }
        emit_rm(e, 0, 1, 0x85, -1, b->reg, a, 0);
        return 1;
    case X_IMUL:
        if (a->kind != OPD_REG || !is_rm(b)) {
            return 0;
        }
        emit_rm(e, 0, 1, 0x0F, 0xAF, a->reg, b, 0);
        return 1;
    case X_IDIV:
    case X_DIV:
    case X_NEG:
    case X_NOT: {
        int ext = in->op == X_IDIV ? 7 : in->op == X_DIV ? 6 : in->op == X_NEG ? 3 : 2;
        if (!is_rm(a)) {
            return 0;
        }
        emit_rm(e, 0, 1, 0xF7, -1, ext, a, 0);
        return 1;
    }
    case X_CQO:
        put(e, 0x48);
        put(e, 0x99);
        return 1;
    case X_SHL:
    case X_SHR:
    case X_SAR: {
        int ext = in->op == X_SHL ? 4 : in->op == X_SHR ? 5 : 7;
        if (!is_rm(a)) {
            return 0;
        }
        if (b->kind == OPD_REG && b->reg == RCX) {
            emit_rm(e, 0, 1, 0xD3, -1, ext, a, 0);
            return 1;
        }
        if (b->kind == OPD_IMM && b->imm == 1) {
            emit_rm(e, 0, 1, 0xD1, -1, ext, a, 0); /* shift by one: no immediate byte */
            return 1;
        }
        if (b->kind == OPD_IMM && b->imm >= 0 && b->imm <= 63) {
            emit_rm(e, 0, 1, 0xC1, -1, ext, a, 0);
            put(e, (int)b->imm);
            return 1;
        }
        return 0;
    }
    case X_PUSH:
        if (a->kind == OPD_REG) {
            if (a->reg >= 8) {
                put(e, 0x41);
            }
            put(e, 0x50 + (a->reg & 7));
            return 1;
        }
        if (a->kind == OPD_IMM && fits32(a->imm)) {
            if (fits8(a->imm)) {
                put(e, 0x6A);
                put(e, (int)(a->imm & 0xFF));
            } else {
                put(e, 0x68);
                put32(e, (long)a->imm);
            }
            return 1;
        }
        return 0;
    case X_POP:
        if (a->kind != OPD_REG) {
            return 0;
        }
        if (a->reg >= 8) {
            put(e, 0x41);
        }
        put(e, 0x58 + (a->reg & 7));
        return 1;
    case X_CALL:
        if (a->kind == OPD_IMPORT) {
            put(e, 0xFF); /* call qword ptr [rip + disp32] */
            put(e, 0x15);
            put32(e, rel);
            return 1;
        }
        put(e, 0xE8);
        put32(e, rel);
        return 1;
    case X_RET:
        put(e, 0xC3);
        return 1;
    case X_JMP:
        if (short_jump) {
            put(e, 0xEB);
            put(e, (int)(rel & 0xFF));
        } else {
            put(e, 0xE9);
            put32(e, rel);
        }
        return 1;
    case X_JCC:
        if (short_jump) {
            put(e, 0x70 + in->cc);
            put(e, (int)(rel & 0xFF));
        } else {
            put(e, 0x0F);
            put(e, 0x80 + in->cc);
            put32(e, rel);
        }
        return 1;
    case X_SETCC:
        if (a->kind != OPD_REG) {
            return 0;
        }
        emit_rm(e, 0, 0, 0x0F, 0x90 + in->cc, 0, a, byte_rex(a));
        return 1;
    case X_SYSCALL:
        put(e, 0x0F);
        put(e, 0x05);
        return 1;
    case X_REP_MOVSB:
        put(e, 0xF3);
        put(e, 0xA4);
        return 1;
    case X_NOP:
        put(e, 0x90);
        return 1;
    case X_LABEL:
        return 1; /* no bytes */
    }
    return 0;
}

size_t inst_encode(const Inst *in, unsigned char *out) {
    Enc e;

    if (in->op == X_CALL || in->op == X_JMP || in->op == X_JCC || in->op == X_LABEL) {
        return 0;
    }
    if (!enc_inst(&e, in, 0, 0)) {
        return 0;
    }
    memcpy(out, e.b, (size_t)e.n);
    return (size_t)e.n;
}

static int is_jump(XOp op) {
    return op == X_JMP || op == X_JCC;
}

int asm_encode(const Asm *a, Arena *arena, Code *out, size_t *label_off, char *err, size_t errcap) {
    size_t n = asm_count(a), nl = asm_label_count(a), i, total;
    size_t *sz = arena_alloc(arena, (n + 1) * sizeof(size_t));
    size_t *off = arena_alloc(arena, (n + 1) * sizeof(size_t));
    unsigned char *is_short = arena_alloc(arena, n + 1);
    unsigned char *defined = arena_alloc(arena, nl + 1);
    size_t *import_slot = arena_alloc(arena, (asm_import_count(a) + 1) * sizeof(size_t));
    int changed;
    Enc e;

    /* sizes with every jump in its long form */
    for (i = 0; i < n; i++) {
        const Inst *in = asm_at(a, i);
        if (!enc_inst(&e, in, 0, 0)) {
            char *text = asm_format_inst(a, in);
            snprintf(err, errcap, "cannot encode instruction %lu: %s", (unsigned long)i, text);
            free(text);
            return 0;
        }
        sz[i] = (size_t)e.n;
    }

    /* shrink jumps that reach their target with an 8-bit displacement */
    do {
        size_t pos = 0;
        changed = 0;
        for (i = 0; i < n; i++) {
            const Inst *in = asm_at(a, i);
            off[i] = pos;
            if (in->op == X_LABEL) {
                label_off[in->a.label] = pos;
                defined[in->a.label] = 1;
            }
            pos += sz[i];
        }
        off[n] = pos;
        for (i = 0; i < n; i++) {
            const Inst *in = asm_at(a, i);
            if (is_jump(in->op) && !is_short[i]) {
                long long disp, target;
                if (!defined[in->a.label]) {
                    snprintf(err, errcap, "label %d is used but not defined", in->a.label);
                    return 0;
                }
                /* a target after this jump moves closer by what the jump itself saves */
                target = (long long)label_off[in->a.label];
                if (target > (long long)off[i]) {
                    target -= (long long)(sz[i] - 2);
                }
                disp = target - (long long)(off[i] + 2);
                if (fits8(disp)) {
                    is_short[i] = 1;
                    sz[i] = 2;
                    changed = 1;
                }
            }
        }
    } while (changed);

    total = off[n];
    out->import_base = (total + 7) / 8 * 8;
    asm_import_slots(a, import_slot);
    out->data = arena_alloc(arena, total + 1);
    out->len = 0;
    for (i = 0; i < n; i++) {
        const Inst *in = asm_at(a, i);
        long rel = 0;

        if (in->op == X_CALL && in->a.kind == OPD_IMPORT) {
            rel = (long)((long long)(out->import_base + 8 * import_slot[in->a.label]) -
                         (long long)(off[i] + sz[i]));
        } else if (is_jump(in->op) || in->op == X_CALL) {
            if (!defined[in->a.label]) {
                snprintf(err, errcap, "label %d is used but not defined", in->a.label);
                return 0;
            }
            rel = (long)((long long)label_off[in->a.label] - (long long)(off[i] + sz[i]));
        }
        enc_inst(&e, in, is_short[i], rel);
        if ((size_t)e.n != sz[i]) {
            snprintf(err, errcap, "internal error: size changed for instruction %lu", (unsigned long)i);
            return 0;
        }
        memcpy(out->data + out->len, e.b, (size_t)e.n);
        out->len += (size_t)e.n;
    }
    return 1;
}
