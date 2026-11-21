#include "asm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void asm_init(Asm *a, Arena *arena) {
    a->arena = arena;
    vec_init(&a->insts, sizeof(Inst));
    vec_init(&a->label_names, sizeof(const char *));
    vec_init(&a->imports, sizeof(Import));
}

int asm_import(Asm *a, const char *dll, const char *name) {
    size_t i;
    Import imp;
    const Import *all = a->imports.data;

    for (i = 0; i < a->imports.len; i++) {
        if (strcmp(all[i].dll, dll) == 0 && strcmp(all[i].name, name) == 0) {
            return (int)i;
        }
    }
    imp.dll = dll;
    imp.name = name;
    vec_push(a->arena, &a->imports, &imp);
    return (int)a->imports.len - 1;
}

size_t asm_import_count(const Asm *a) {
    return a->imports.len;
}

const Import *asm_import_at(const Asm *a, size_t i) {
    return &((const Import *)a->imports.data)[i];
}

size_t asm_import_slots(const Asm *a, size_t *slots) {
    size_t n = a->imports.len, i, j, next = 0;
    const Import *all = a->imports.data;

    for (i = 0; i < n; i++) {
        int first = 1;
        for (j = 0; j < i; j++) {
            if (strcmp(all[j].dll, all[i].dll) == 0) {
                first = 0;
                break;
            }
        }
        if (!first) {
            continue; /* numbered together with the first import of its DLL */
        }
        for (j = i; j < n; j++) {
            if (strcmp(all[j].dll, all[i].dll) == 0) {
                slots[j] = next++;
            }
        }
        next++; /* the zero slot that ends this DLL's table */
    }
    return next;
}

int asm_new_label(Asm *a, const char *name) {
    vec_push(a->arena, &a->label_names, &name);
    return (int)a->label_names.len - 1;
}

size_t asm_label_count(const Asm *a) {
    return a->label_names.len;
}

const char *asm_label_name(const Asm *a, int label) {
    return ((const char **)a->label_names.data)[label];
}

void asm_emit(Asm *a, Inst in) {
    vec_push(a->arena, &a->insts, &in);
}

size_t asm_count(const Asm *a) {
    return a->insts.len;
}

void asm_set_imm(Asm *a, size_t index, long long value) {
    ((Inst *)a->insts.data)[index].b.imm = value;
}

void asm_remove(Asm *a, size_t index) {
    Inst *d = a->insts.data;
    memmove(d + index, d + index + 1, (a->insts.len - index - 1) * sizeof(Inst));
    a->insts.len--;
}

const Inst *asm_at(const Asm *a, size_t i) {
    return &((const Inst *)a->insts.data)[i];
}

Opd opd_reg(Reg r) {
    Opd o;
    memset(&o, 0, sizeof o);
    o.kind = OPD_REG;
    o.reg = r;
    return o;
}

Opd opd_imm(long long v) {
    Opd o;
    memset(&o, 0, sizeof o);
    o.kind = OPD_IMM;
    o.imm = v;
    return o;
}

Opd opd_mem(Reg base, long disp) {
    Opd o;
    memset(&o, 0, sizeof o);
    o.kind = OPD_MEM;
    o.reg = base;
    o.disp = disp;
    return o;
}

Opd opd_import(int import) {
    Opd o;
    memset(&o, 0, sizeof o);
    o.kind = OPD_IMPORT;
    o.label = import;
    return o;
}

Opd opd_label(int label) {
    Opd o;
    memset(&o, 0, sizeof o);
    o.kind = OPD_LABEL;
    o.label = label;
    return o;
}

Inst inst0(XOp op) {
    Inst in;
    memset(&in, 0, sizeof in);
    in.op = op;
    return in;
}

Inst inst1(XOp op, Opd a) {
    Inst in = inst0(op);
    in.a = a;
    return in;
}

Inst inst2(XOp op, int size, Opd a, Opd b) {
    Inst in = inst0(op);
    in.size = size;
    in.a = a;
    in.b = b;
    return in;
}

Inst inst_cc(XOp op, Cond cc, Opd a) {
    Inst in = inst1(op, a);
    in.cc = cc;
    return in;
}

void asm_op0(Asm *a, XOp op) {
    asm_emit(a, inst0(op));
}

void asm_op1(Asm *a, XOp op, Opd x) {
    asm_emit(a, inst1(op, x));
}

void asm_op2(Asm *a, XOp op, int size, Opd x, Opd y) {
    asm_emit(a, inst2(op, size, x, y));
}

void asm_jcc(Asm *a, Cond cc, int label) {
    asm_emit(a, inst_cc(X_JCC, cc, opd_label(label)));
}

void asm_setcc(Asm *a, Cond cc, Reg r) {
    Inst in = inst_cc(X_SETCC, cc, opd_reg(r));
    in.size = 1;
    asm_emit(a, in);
}

void asm_label(Asm *a, int label) {
    asm_emit(a, inst1(X_LABEL, opd_label(label)));
}

/* ---- text form ---- */

static const char *REG64[16] = {"rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi",
                                "r8",  "r9",  "r10", "r11", "r12", "r13", "r14", "r15"};
static const char *REG32[16] = {"eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi",
                                "r8d", "r9d", "r10d", "r11d", "r12d", "r13d", "r14d", "r15d"};
static const char *REG16[16] = {"ax", "cx", "dx", "bx", "sp", "bp", "si", "di",
                                "r8w", "r9w", "r10w", "r11w", "r12w", "r13w", "r14w", "r15w"};
static const char *REG8[16] = {"al", "cl", "dl", "bl", "spl", "bpl", "sil", "dil",
                               "r8b", "r9b", "r10b", "r11b", "r12b", "r13b", "r14b", "r15b"};

static const char *reg_name(Reg r, int size) {
    switch (size) {
    case 1: return REG8[r];
    case 2: return REG16[r];
    case 4: return REG32[r];
    default: return REG64[r];
    }
}

static const char *mem_prefix(int size) {
    switch (size) {
    case 1: return "byte ptr ";
    case 2: return "word ptr ";
    case 4: return "dword ptr ";
    default: return "qword ptr ";
    }
}

static const char *cond_name(Cond cc) {
    switch (cc) {
    case CC_B: return "b";
    case CC_AE: return "ae";
    case CC_E: return "e";
    case CC_NE: return "ne";
    case CC_BE: return "be";
    case CC_A: return "a";
    case CC_L: return "l";
    case CC_GE: return "ge";
    case CC_LE: return "le";
    case CC_G: return "g";
    }
    return "?";
}

static const char *mnemonic(XOp op) {
    switch (op) {
    case X_MOV: return "mov";
    case X_MOVZX: return "movzx";
    case X_MOVSX: return "movsx";
    case X_MOVSXD: return "movsxd";
    case X_LEA: return "lea";
    case X_ADD: return "add";
    case X_OR: return "or";
    case X_AND: return "and";
    case X_SUB: return "sub";
    case X_XOR: return "xor";
    case X_CMP: return "cmp";
    case X_TEST: return "test";
    case X_IMUL: return "imul";
    case X_IDIV: return "idiv";
    case X_DIV: return "div";
    case X_NEG: return "neg";
    case X_NOT: return "not";
    case X_CQO: return "cqo";
    case X_SHL: return "shl";
    case X_SHR: return "shr";
    case X_SAR: return "sar";
    case X_PUSH: return "push";
    case X_POP: return "pop";
    case X_CALL: return "call";
    case X_RET: return "ret";
    case X_LEAVE: return "leave";
    case X_JMP: return "jmp";
    case X_SYSCALL: return "syscall";
    case X_REP_MOVSB: return "rep movsb";
    case X_NOP: return "nop";
    default: return "?";
    }
}

/* Appends one operand. `size` is the width used to print registers and memory. */
static void put_opd(char *buf, size_t cap, const Asm *a, const Opd *o, int size, int with_ptr) {
    size_t used = strlen(buf);
    char *w = buf + used;
    size_t room = cap - used;

    switch (o->kind) {
    case OPD_REG:
        snprintf(w, room, "%s", reg_name(o->reg, size));
        break;
    case OPD_IMM:
        snprintf(w, room, "%lld", o->imm);
        break;
    case OPD_MEM: {
        const char *pre = with_ptr ? mem_prefix(size) : "";
        if (o->disp == 0) {
            snprintf(w, room, "%s[%s]", pre, REG64[o->reg]);
        } else if (o->disp > 0) {
            snprintf(w, room, "%s[%s+%ld]", pre, REG64[o->reg], o->disp);
        } else {
            snprintf(w, room, "%s[%s-%ld]", pre, REG64[o->reg], -o->disp);
        }
        break;
    }
    case OPD_LABEL: {
        const char *name = asm_label_name(a, o->label);
        if (name) {
            snprintf(w, room, "%s", name);
        } else {
            snprintf(w, room, ".L%d", o->label);
        }
        break;
    }
    case OPD_IMPORT:
        snprintf(w, room, "qword ptr [rip+%s]", asm_import_at(a, o->label)->name);
        break;
    case OPD_NONE:
        break;
    }
}

char *asm_format_inst(const Asm *a, const Inst *in) {
    char buf[200];
    int sz = in->size ? in->size : 8;
    char *out;

    buf[0] = '\0';
    switch (in->op) {
    case X_LABEL: {
        const char *name = asm_label_name(a, in->a.label);
        if (name) {
            snprintf(buf, sizeof buf, "%s:", name);
        } else {
            snprintf(buf, sizeof buf, ".L%d:", in->a.label);
        }
        break;
    }
    case X_JCC:
        snprintf(buf, sizeof buf, "j%s ", cond_name(in->cc));
        put_opd(buf, sizeof buf, a, &in->a, 8, 0);
        break;
    case X_SETCC:
        snprintf(buf, sizeof buf, "set%s ", cond_name(in->cc));
        put_opd(buf, sizeof buf, a, &in->a, 1, 1);
        break;
    case X_MOVZX:
        snprintf(buf, sizeof buf, "movzx ");
        put_opd(buf, sizeof buf, a, &in->a, 4, 0);
        strcat(buf, ", ");
        put_opd(buf, sizeof buf, a, &in->b, sz, 1);
        break;
    case X_MOVSX:
        snprintf(buf, sizeof buf, "movsx ");
        put_opd(buf, sizeof buf, a, &in->a, 8, 0);
        strcat(buf, ", ");
        put_opd(buf, sizeof buf, a, &in->b, sz, 1);
        break;
    case X_MOVSXD:
        snprintf(buf, sizeof buf, "movsxd ");
        put_opd(buf, sizeof buf, a, &in->a, 8, 0);
        strcat(buf, ", ");
        put_opd(buf, sizeof buf, a, &in->b, 4, 1);
        break;
    case X_LEA:
        snprintf(buf, sizeof buf, "lea ");
        put_opd(buf, sizeof buf, a, &in->a, 8, 0);
        strcat(buf, ", ");
        put_opd(buf, sizeof buf, a, &in->b, 8, 0);
        break;
    case X_SHL:
    case X_SHR:
    case X_SAR:
        snprintf(buf, sizeof buf, "%s ", mnemonic(in->op));
        put_opd(buf, sizeof buf, a, &in->a, 8, 1);
        strcat(buf, ", ");
        put_opd(buf, sizeof buf, a, &in->b, 1, 1); /* cl or an immediate */
        break;
    default:
        snprintf(buf, sizeof buf, "%s", mnemonic(in->op));
        if (in->a.kind != OPD_NONE) {
            strcat(buf, " ");
            put_opd(buf, sizeof buf, a, &in->a, sz, 1);
        }
        if (in->b.kind != OPD_NONE) {
            strcat(buf, ", ");
            put_opd(buf, sizeof buf, a, &in->b, sz, 1);
        }
        break;
    }
    out = malloc(strlen(buf) + 1);
    if (!out) {
        exit(1);
    }
    strcpy(out, buf);
    return out;
}

char *asm_format(const Asm *a) {
    size_t i, cap = 256, len = 0;
    char *out = malloc(cap);

    if (!out) {
        exit(1);
    }
    out[0] = '\0';
    for (i = 0; i < asm_count(a); i++) {
        const Inst *in = asm_at(a, i);
        char *line = asm_format_inst(a, in);
        size_t need = strlen(line) + 6;
        if (len + need >= cap) {
            while (len + need >= cap) {
                cap *= 2;
            }
            out = realloc(out, cap);
            if (!out) {
                exit(1);
            }
        }
        len += (size_t)sprintf(out + len, "%s%s\n", in->op == X_LABEL ? "" : "    ", line);
        free(line);
    }
    return out;
}
