#!/usr/bin/env python3
"""Differential test for integer arithmetic.

Generates random Gale programs that mix integer types, operators, casts, comparisons
and if-expressions, compiles each with `gale build`, runs it, and compares the exit
status with the result of an independent evaluation in Python (fixed-width integers
that wrap around, division that truncates toward zero, ...).

usage: fuzz_arith.py [path-to-gale] [count] [seed]
Linux x86-64 only (it runs the compiled programs).
"""
import os
import random
import subprocess
import sys
import tempfile

TYPES = {  # name: (bits, signed)
    "i8": (8, True), "i16": (16, True), "i32": (32, True), "i64": (64, True), "isize": (64, True),
    "u8": (8, False), "u16": (16, False), "u32": (32, False), "u64": (64, False), "usize": (64, False),
}
NAMES = list(TYPES)


def wrap(v, t):
    bits, signed = TYPES[t]
    v &= (1 << bits) - 1
    if signed and v >= 1 << (bits - 1):
        v -= 1 << bits
    return v


def trunc_div(a, b):
    q = abs(a) // abs(b)
    return q if (a < 0) == (b < 0) else -q


class Gen:
    def __init__(self, rng):
        self.rng = rng
        self.vars = {}   # type -> [(name, value)]
        self.count = 0

    def lit(self, t):
        r = self.rng
        v = r.choice([0, 1, 2, 3, 7, 100, 127, 128, 255, 256, 1000, 32767, 65535, -1, -2, -5, -128, -129,
                      2147483647, -2147483648, r.randint(-2**31, 2**31 - 1)])
        return "((%s) as %s)" % (v if v >= 0 else "-%d" % -v, t), wrap(v, t)

    def expr(self, t, depth):
        r = self.rng
        if depth <= 0 or r.random() < 0.15:
            have = self.vars.get(t, [])
            if have and r.random() < 0.6:
                name, val = r.choice(have)
                return name, val
            return self.lit(t)
        kind = r.choice(["bin", "bin", "bin", "shift", "divmod", "unary", "cast", "cmp", "if"])
        bits, signed = TYPES[t]
        if kind == "bin":
            op = r.choice(["+", "-", "*", "&", "|", "^"])
            a, av = self.expr(t, depth - 1)
            b, bv = self.expr(t, depth - 1)
            v = {"+": av + bv, "-": av - bv, "*": av * bv, "&": av & bv, "|": av | bv, "^": av ^ bv}[op]
            return "(%s %s %s)" % (a, op, b), wrap(v, t)
        if kind == "shift":
            op = r.choice(["<<", ">>"])
            a, av = self.expr(t, depth - 1)
            ct = r.choice(NAMES)
            c, cv = self.expr(ct, depth - 1)
            n = cv & 7
            v = av << n if op == "<<" else av >> n
            return "(%s %s (%s & 7))" % (a, op, c), wrap(v, t)
        if kind == "divmod":
            op = r.choice(["/", "%"])
            a, av = self.expr(t, depth - 1)
            b, bv = self.expr(t, depth - 1)
            d = (bv & 6) + 3            # 3, 5, 7 or 9: never zero or -1
            q = trunc_div(av, d)
            v = q if op == "/" else av - q * d
            return "(%s %s ((%s & 6) + 3))" % (a, op, b), wrap(v, t)
        if kind == "unary":
            a, av = self.expr(t, depth - 1)
            if signed and r.random() < 0.5:
                return "(-%s)" % a, wrap(-av, t)
            return "(~%s)" % a, wrap(~av, t)
        if kind == "cast":
            st = r.choice(NAMES)
            a, av = self.expr(st, depth - 1)
            return "(%s as %s)" % (a, t), wrap(av, t)
        if kind == "cmp":
            ct = r.choice(NAMES)
            op = r.choice(["==", "!=", "<", "<=", ">", ">="])
            a, av = self.expr(ct, depth - 1)
            b, bv = self.expr(ct, depth - 1)
            v = {"==": av == bv, "!=": av != bv, "<": av < bv, "<=": av <= bv, ">": av > bv, ">=": av >= bv}[op]
            return "((%s %s %s) as %s)" % (a, op, b, t), int(v)
        # if-expression
        ct = r.choice(NAMES)
        a, av = self.expr(ct, depth - 1)
        b, bv = self.expr(ct, depth - 1)
        x, xv = self.expr(t, depth - 1)
        y, yv = self.expr(t, depth - 1)
        cond = av < bv
        return "(if %s < %s { %s } else { %s })" % (a, b, x, y), (xv if cond else yv)


def program(rng):
    g = Gen(rng)
    lines = ["func main() -> i32 = {"]
    checks = []
    for i in range(rng.randint(2, 5)):
        t = rng.choice(NAMES)
        e, v = g.expr(t, rng.randint(1, 4))
        name = "v%d" % i
        lines.append("    let %s: %s = %s" % (name, t, e))
        g.vars.setdefault(t, []).append((name, v))
        checks.append((name, t, v))
    # mix everything into one i64, then take it as an exit status
    total = 0
    terms = []
    for k, (name, t, v) in enumerate(checks):
        mult = 31 + 2 * k
        terms.append("((%s as i64) * %d)" % (name, mult))
        total = wrap(total + wrap(v * mult, "i64"), "i64")
    lines.append("    (%s) as i32" % " + ".join(terms))
    lines.append("}")
    return "\n".join(lines) + "\n", wrap(total, "i32") & 255


def main():
    gale = sys.argv[1] if len(sys.argv) > 1 else "build/gale"
    count = int(sys.argv[2]) if len(sys.argv) > 2 else 300
    seed = int(sys.argv[3]) if len(sys.argv) > 3 else 1
    rng = random.Random(seed)
    bad = 0
    with tempfile.TemporaryDirectory() as d:
        src = os.path.join(d, "p.gale")
        exe = os.path.join(d, "p")
        for n in range(count):
            text, want = program(rng)
            with open(src, "w") as f:
                f.write(text)
            r = subprocess.run([gale, "build", src, "-o", exe], capture_output=True, text=True)
            if r.returncode != 0:
                bad += 1
                print("COMPILE ERROR in program %d:\n%s\n%s" % (n, text, r.stderr))
            else:
                got = subprocess.run([exe]).returncode
                if got != want:
                    bad += 1
                    print("MISMATCH in program %d: got %d, expected %d\n%s" % (n, got, want, text))
            if bad >= 3:
                break
    print("programs: %d, failures: %d (seed %d)" % (n + 1, bad, seed))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
