#!/usr/bin/env python3
"""Generates tests/encode_cases.inc and tests/encode_seqs.inc.

Every case is written once, as Intel-syntax text. This script
  * parses the text into the C code that builds the same instruction,
  * assembles the text with GNU as (the reference) to get the expected bytes.
test_encode.c then checks that our printer reproduces the text and our encoder
reproduces the bytes. Needs `as` and `objcopy` (binutils); the generated files are
committed, so only people changing the encoder need to run this.
"""
import os
import re
import subprocess
import sys
import tempfile

REGS64 = ["rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi",
          "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15"]
REGS32 = ["eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi",
          "r8d", "r9d", "r10d", "r11d", "r12d", "r13d", "r14d", "r15d"]
REGS16 = ["ax", "cx", "dx", "bx", "sp", "bp", "si", "di",
          "r8w", "r9w", "r10w", "r11w", "r12w", "r13w", "r14w", "r15w"]
REGS8 = ["al", "cl", "dl", "bl", "spl", "bpl", "sil", "dil",
         "r8b", "r9b", "r10b", "r11b", "r12b", "r13b", "r14b", "r15b"]
REGMAP = {}
for table, width in ((REGS64, 8), (REGS32, 4), (REGS16, 2), (REGS8, 1)):
    for i, name in enumerate(table):
        REGMAP[name] = (i, width)

WIDTH = {"byte": 1, "word": 2, "dword": 4, "qword": 8}
CC = {"b": "CC_B", "ae": "CC_AE", "e": "CC_E", "ne": "CC_NE", "be": "CC_BE", "a": "CC_A",
      "l": "CC_L", "ge": "CC_GE", "le": "CC_LE", "g": "CC_G"}
ALU = {"add": "X_ADD", "or": "X_OR", "and": "X_AND", "sub": "X_SUB", "xor": "X_XOR", "cmp": "X_CMP"}
UNARY = {"idiv": "X_IDIV", "div": "X_DIV", "neg": "X_NEG", "not": "X_NOT"}
SHIFT = {"shl": "X_SHL", "shr": "X_SHR", "sar": "X_SAR"}
NOARG = {"ret": "X_RET", "leave": "X_LEAVE", "syscall": "X_SYSCALL", "cqo": "X_CQO", "nop": "X_NOP",
         "rep movsb": "X_REP_MOVSB"}


def parse_operand(s):
    s = s.strip()
    m = re.fullmatch(r"(?:(byte|word|dword|qword) ptr )?\[(\w+)(?:([+-])(\d+))?\]", s)
    if m:
        width = WIDTH[m.group(1)] if m.group(1) else None
        disp = int(m.group(4)) if m.group(4) else 0
        if m.group(3) == "-":
            disp = -disp
        return ("mem", width, REGMAP[m.group(2)][0], disp)
    if s in REGMAP:
        idx, width = REGMAP[s]
        return ("reg", width, idx)
    return ("imm", int(s, 0))


def c_opd(o):
    if o[0] == "reg":
        return "REG(%s)" % REGS64[o[2]].upper()
    if o[0] == "mem":
        return "MEM(%s, %d)" % (REGS64[o[2]].upper(), o[3])
    return "IMM(%dLL)" % o[1]


def opd_width(o):
    return o[1] if o[0] in ("reg", "mem") else None


def parse_inst(text):
    """Intel text -> C expression building the Inst."""
    if text in NOARG:
        return "inst0(%s)" % NOARG[text]
    mn, _, rest = text.partition(" ")
    ops = [parse_operand(x) for x in rest.split(",")] if rest else []
    if mn == "mov":
        size = next(opd_width(o) for o in ops if opd_width(o))
        return "inst2(X_MOV, %d, %s, %s)" % (size, c_opd(ops[0]), c_opd(ops[1]))
    if mn in ("movzx", "movsx"):
        return "inst2(%s, %d, %s, %s)" % ("X_MOVZX" if mn == "movzx" else "X_MOVSX",
                                         opd_width(ops[1]), c_opd(ops[0]), c_opd(ops[1]))
    if mn == "movsxd":
        return "inst2(X_MOVSXD, 4, %s, %s)" % (c_opd(ops[0]), c_opd(ops[1]))
    if mn == "lea":
        return "inst2(X_LEA, 8, %s, %s)" % (c_opd(ops[0]), c_opd(ops[1]))
    if mn in ALU:
        return "inst2(%s, 8, %s, %s)" % (ALU[mn], c_opd(ops[0]), c_opd(ops[1]))
    if mn == "test":
        return "inst2(X_TEST, 8, %s, %s)" % (c_opd(ops[0]), c_opd(ops[1]))
    if mn == "imul":
        return "inst2(X_IMUL, 8, %s, %s)" % (c_opd(ops[0]), c_opd(ops[1]))
    if mn in UNARY:
        return "inst1(%s, %s)" % (UNARY[mn], c_opd(ops[0]))
    if mn in SHIFT:
        return "inst2(%s, 8, %s, %s)" % (SHIFT[mn], c_opd(ops[0]), c_opd(ops[1]))
    if mn == "push":
        return "inst1(X_PUSH, %s)" % c_opd(ops[0])
    if mn == "pop":
        return "inst1(X_POP, %s)" % c_opd(ops[0])
    if mn.startswith("set"):
        return "inst_cc(X_SETCC, %s, %s)" % (CC[mn[3:]], c_opd(ops[0]))
    raise ValueError("cannot parse: " + text)


def assemble(lines):
    """Assemble Intel-syntax lines with GNU as; returns the .text bytes as 'xx xx ..'."""
    with tempfile.TemporaryDirectory() as d:
        src = os.path.join(d, "t.s")
        obj = os.path.join(d, "t.o")
        binf = os.path.join(d, "t.bin")
        with open(src, "w") as f:
            f.write(".intel_syntax noprefix\n.text\n" + "\n".join(lines) + "\n")
        subprocess.run(["as", "--64", "-o", obj, src], check=True)
        subprocess.run(["objcopy", "-O", "binary", "--only-section=.text", obj, binf], check=True)
        data = open(binf, "rb").read()
    return " ".join("%02x" % b for b in data)


def gas_equivalent(text):
    """Where we deliberately pick a different (shorter) encoding than GNU as, give the
    instruction GNU as needs to be told in order to produce the same bytes."""
    m = re.fullmatch(r"mov (r\w+), (\d+|0x[0-9a-f]+)", text)
    if m and m.group(1) in REGMAP and REGMAP[m.group(1)][1] == 8:
        v = int(m.group(2), 0)
        if 0 <= v <= 0xFFFFFFFF:
            return "mov %s, %s" % (REGS32[REGMAP[m.group(1)][0]], m.group(2))
    return text


def inst_cases():
    c = []
    # mov reg, reg
    for a, b in [("rax", "rbx"), ("r8", "rax"), ("rax", "r9"), ("r12", "r13"), ("rsp", "rbp"), ("rbp", "rsp"), ("rdi", "rsi")]:
        c.append("mov %s, %s" % (a, b))
    c.append("mov eax, ecx")
    # mov reg, imm
    for r in ("rax", "rcx", "r8", "r15"):
        for v in (0, 1, 255, 2147483647, 4294967295, -1, -2147483648):
            c.append("mov %s, %d" % (r, v))
    for r in ("rax", "r9"):
        for v in (4294967296, 9223372036854775807, -9223372036854775807, -2147483649):
            c.append("mov %s, %d" % (r, v))
    # loads
    for mem in ("[rbp-8]", "[rbp+16]", "[rax]", "[rsp]", "[r12]", "[r13]", "[rbp-128]", "[rbp-129]",
                "[rbp+127]", "[rbp+128]", "[rcx+1000]", "[rsp+8]", "[r12+8]", "[r13-8]", "[rbp-2147483648]"):
        c.append("mov rax, qword ptr %s" % mem)
    c.append("mov r8, qword ptr [rbp-200]")
    c.append("mov r15, qword ptr [r14+8]")
    c.append("mov eax, dword ptr [rbp-8]")
    c.append("mov r9d, dword ptr [rax]")
    # stores
    for sz, reg in (("qword", "rax"), ("dword", "eax"), ("word", "ax"), ("byte", "al")):
        c.append("mov %s ptr [rbp-8], %s" % (sz, reg))
        c.append("mov %s ptr [rax+4], %s" % (sz, reg))
    c.append("mov byte ptr [rax], sil")
    c.append("mov byte ptr [rbp-1], dil")
    c.append("mov byte ptr [r9], cl")
    c.append("mov byte ptr [rax], r8b")
    c.append("mov qword ptr [r8], r9")
    c.append("mov word ptr [rsp+2], r10w")
    # store immediates
    for sz, v in (("qword", 5), ("qword", -5), ("qword", 100000), ("dword", 5), ("dword", -1),
                  ("word", 5), ("word", 1000), ("byte", 5), ("byte", 127), ("byte", -1)):
        c.append("mov %s ptr [rbp-8], %d" % (sz, v))
    c.append("mov qword ptr [r12+8], 7")
    # zero / sign extension
    for src in ("byte ptr [rbp-8]", "word ptr [rbp-8]", "byte ptr [rax]", "byte ptr [r9+4]"):
        c.append("movzx eax, %s" % src)
        c.append("movsx rax, %s" % src)
    c.append("movzx r8d, byte ptr [rax]")
    c.append("movsx r9, word ptr [rcx]")
    for a, b in [("eax", "al"), ("eax", "cl"), ("eax", "sil"), ("eax", "dil"), ("ecx", "r8b"), ("eax", "ax"), ("r9d", "r10w")]:
        c.append("movzx %s, %s" % (a, b))
    for a, b in [("rax", "al"), ("rax", "cx"), ("rax", "sil"), ("r8", "r9b"), ("rcx", "dx")]:
        c.append("movsx %s, %s" % (a, b))
    c.append("movsxd rax, dword ptr [rbp-8]")
    c.append("movsxd rax, eax")
    c.append("movsxd r8, ecx")
    c.append("movsxd rcx, dword ptr [r9+4]")
    # lea
    for t in ("lea rax, [rbp-8]", "lea rsi, [rbp+16]", "lea rdi, [rax]", "lea rax, [rsp+8]",
              "lea r8, [r12+4]", "lea rcx, [rbp-1000]"):
        c.append(t)
    # ALU
    for op in ("add", "or", "and", "sub", "xor", "cmp"):
        c.append("%s rax, rcx" % op)
        c.append("%s r8, rax" % op)
        c.append("%s rax, r9" % op)
        c.append("%s rax, 5" % op)
        c.append("%s rax, -1" % op)
        c.append("%s rax, 200" % op)
        c.append("%s rcx, -200" % op)
        c.append("%s r10, 100000" % op)
        c.append("%s rax, qword ptr [rbp-8]" % op)
        c.append("%s qword ptr [rbp-8], rax" % op)
        c.append("%s qword ptr [rax], 5" % op)
        c.append("%s qword ptr [rbp-16], 1000" % op)
    c.append("sub rsp, 16")
    c.append("sub rsp, 200")
    c.append("add rsp, 8")
    c.append("test rax, rax")
    c.append("test rax, rcx")
    c.append("test r8, r9")
    c.append("imul rax, rcx")
    c.append("imul r8, r9")
    c.append("imul rax, qword ptr [rbp-8]")
    for op in ("idiv", "div", "neg", "not"):
        c.append("%s rcx" % op)
        c.append("%s r8" % op)
        c.append("%s qword ptr [rbp-8]" % op)
    c.append("cqo")
    for op in ("shl", "shr", "sar"):
        c.append("%s rax, cl" % op)
        c.append("%s r8, cl" % op)
        c.append("%s rax, 1" % op)
        c.append("%s rax, 3" % op)
        c.append("%s r8, 63" % op)
        c.append("%s qword ptr [rbp-8], cl" % op)
    for r in ("rax", "rcx", "rbp", "r8", "r15"):
        c.append("push %s" % r)
        c.append("pop %s" % r)
    c.append("push 5")
    c.append("push -1")
    c.append("push 200")
    for cc in CC:
        c.append("set%s al" % cc)
    c.append("setne sil")
    c.append("setg r8b")
    c.append("setl cl")
    for t in NOARG:
        c.append(t)
    return c


def build_inst_cases():
    out = ["/* Generated by tools/gen_encode_cases.py -- do not edit. */"]
    cases = inst_cases()
    # assemble in one go: one line per instruction, then split by assembling each (sizes vary)
    for text in cases:
        hexbytes = assemble([gas_equivalent(text)])
        out.append('CASE("%s", "%s", %s);' % (text, hexbytes, parse_inst(text)))
    return "\n".join(out) + "\n"


# ---- sequences with labels ----

def nops(n):
    return ["nop"] * n


def seq_defs():
    s = []
    s.append(("jmp forward, short", ["jmp L1", "add rax, 1", "L1:", "ret"]))
    s.append(("jmp backward, short", ["L1:", "add rax, 1", "jmp L1"]))
    s.append(("jmp forward, long", ["jmp L1"] + ["add rax, 1"] * 40 + ["L1:", "ret"]))
    s.append(("jmp backward, long", ["L1:"] + ["add rax, 1"] * 40 + ["jmp L1"]))
    s.append(("jmp forward, 127 bytes", ["jmp L1"] + nops(127) + ["L1:", "ret"]))
    s.append(("jmp forward, 128 bytes", ["jmp L1"] + nops(128) + ["L1:", "ret"]))
    s.append(("jmp backward, -128", ["L1:"] + nops(126) + ["jmp L1"]))
    s.append(("jmp backward, -129", ["L1:"] + nops(127) + ["jmp L1"]))
    s.append(("jmp to next instruction", ["jmp L1", "L1:", "ret"]))
    s.append(("jmp to self", ["L1:", "jmp L1"]))
    for cc in CC:
        s.append(("j%s forward" % cc, ["cmp rax, rcx", "j%s L1" % cc, "add rax, 1", "L1:", "ret"]))
    s.append(("jl backward", ["L1:", "add rax, 1", "cmp rax, rcx", "jl L1", "ret"]))
    s.append(("jcc long forward", ["cmp rax, rcx", "je L1"] + nops(200) + ["L1:", "ret"]))
    s.append(("jcc long backward", ["L1:"] + nops(200) + ["cmp rax, rcx", "jne L1", "ret"]))
    s.append(("call forward", ["call L1", "ret", "L1:", "ret"]))
    s.append(("call backward", ["L1:", "ret", "call L1"]))
    s.append(("call far", ["call L1"] + nops(300) + ["L1:", "ret"]))
    s.append(("two labels, same place", ["jmp L2", "L1:", "L2:", "ret", "jmp L1"]))
    # shrinking one jump lets another one shrink: 63 + 63 bytes of padding between them
    s.append(("cascade", ["jmp L2", "jmp L1"] + nops(120) + ["L1:", "jmp L3"] + nops(2) + ["L2:", "L3:", "ret"]))
    s.append(("nested loops", [
        "L1:", "cmp rax, rcx", "jge L4", "mov rdx, 0",
        "L2:", "cmp rdx, rcx", "jge L3", "add rdx, 1", "jmp L2",
        "L3:", "add rax, 1", "jmp L1", "L4:", "ret"]))
    s.append(("function with a loop", [
        "push rbp", "mov rbp, rsp", "sub rsp, 16", "mov qword ptr [rbp-8], 0",
        "L1:", "mov rax, qword ptr [rbp-8]", "cmp rax, 10", "jge L2",
        "add qword ptr [rbp-8], 1", "jmp L1",
        "L2:", "mov rax, qword ptr [rbp-8]", "mov rsp, rbp", "pop rbp", "ret"]))
    return s


def build_seqs():
    out = ["/* Generated by tools/gen_encode_cases.py -- do not edit. */"]
    table = []
    for k, (name, lines) in enumerate(seq_defs()):
        labels = []
        for ln in lines:
            for m in re.finditer(r"\bL\d+\b", ln):
                if m.group(0) not in labels:
                    labels.append(m.group(0))
        gas = []
        for ln in lines:
            g = re.sub(r"\bL(\d+)\b", r".LL\1", ln)
            gas.append(gas_equivalent(g))
        hexbytes = assemble(gas)
        out.append("static void seq_%d(Asm *a) {" % k)
        for lab in labels:
            out.append("    int %s = asm_new_label(a, NULL);" % lab)
        for ln in lines:
            if ln.endswith(":"):
                out.append("    asm_label(a, %s);" % ln[:-1])
                continue
            mn, _, rest = ln.partition(" ")
            if mn == "jmp":
                out.append("    asm_op1(a, X_JMP, opd_label(%s));" % rest)
            elif mn == "call":
                out.append("    asm_op1(a, X_CALL, opd_label(%s));" % rest)
            elif mn.startswith("j") and mn[1:] in CC:
                out.append("    asm_jcc(a, %s, %s);" % (CC[mn[1:]], rest))
            else:
                out.append("    asm_emit(a, %s);" % parse_inst(ln))
        out.append("}")
        table.append('    {"%s", "%s", seq_%d},' % (name, hexbytes, k))
    out.append("")
    out.append("static const SeqCase SEQS[] = {")
    out.extend(table)
    out.append("};")
    return "\n".join(out) + "\n"


def main():
    root = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tests")
    with open(os.path.join(root, "encode_cases.inc"), "w") as f:
        f.write(build_inst_cases())
    with open(os.path.join(root, "encode_seqs.inc"), "w") as f:
        f.write(build_seqs())
    print("generated", len(inst_cases()), "instruction cases and", len(seq_defs()), "sequences")


if __name__ == "__main__":
    sys.exit(main())
