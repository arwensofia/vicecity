#!/usr/bin/env python3
"""Resolve constant call arguments from a DumpListing.java text dump.

Works on the text produced by tools/ghidra/DumpListing.java (whole program) plus the
ELF (for initialised data and strings). Run with `python3 -I`.

  callargs.py <listing.txt> <elf> calls <target>[,<target>...]   resolve a0-a3/t0-t3 at
                                                                 every call site
  callargs.py <listing.txt> <elf> func <addr>                    print one function
  callargs.py <listing.txt> <elf> callers <addr>                 direct callers
  callargs.py <listing.txt> <elf> tree <addr> [depth]            static call tree
  callargs.py <listing.txt> <elf> codeptrs                       code addresses taken by
                                                                 lui/addiu in another function
                                                                 that are not function starts
                                                                 (thread entries, handlers,
                                                                 callbacks the recompiler needs)

Targets/addresses are hex. The resolver is a forward constant propagator over each
function in address order: it understands lui/addiu/ori/li/move/addu/daddu/or/sll/lw/sw
(incl. $gp-relative and $sp-relative stack slots), forgets everything at branch
targets, and clobbers caller-saved registers after calls. For an argument that points
at a stack slot it prints the words stored there (e.g. ThreadParam / SemaParam), and
for an argument that points into the ELF it prints the C string there, if any.
"""
import re
import struct
import sys

GP = 0x4B1DF0          # set by crt0 (entry: lui a0,0x4b; addiu a0,a0,0x1df0; move gp,a0)
ARGS = ["a0", "a1", "a2", "a3", "t0", "t1", "t2", "t3"]
CALLER_SAVED = {"at", "v0", "v1", "a0", "a1", "a2", "a3", "t0", "t1", "t2", "t3",
                "t4", "t5", "t6", "t7", "t8", "t9", "ra"}
LINE = re.compile(r"^([0-9a-f]{8})  (_?)(\S+)\s*([^;]*?)\s*(?:;(.*))?$")
FUNC = re.compile(r"^==== FUNC ([0-9a-f]{8}) (\S+) size=(\d+)")


def u32(x):
    return x & 0xFFFFFFFF


def s16(x):
    x &= 0xFFFF
    return x - 0x10000 if x & 0x8000 else x


def parse_int(s):
    s = s.strip()
    neg = s.startswith("-")
    if neg:
        s = s[1:]
    v = int(s, 16) if s.startswith("0x") else int(s)
    return -v if neg else v


def norm(r):
    r = r.strip()
    for suf in ("_lo", "_hi", "_qw"):
        if r.endswith(suf):
            r = r[: -len(suf)]
    return r


class Elf:
    def __init__(self, path):
        d = open(path, "rb").read()
        self.d = d
        phoff = struct.unpack_from("<I", d, 28)[0]
        n = struct.unpack_from("<H", d, 44)[0]
        self.segs = []
        for i in range(n):
            p = struct.unpack_from("<8I", d, phoff + 32 * i)
            if p[0] == 1 and p[4]:
                self.segs.append((p[2], p[4], p[1]))

    def off(self, a):
        for va, sz, off in self.segs:
            if va <= a < va + sz:
                return off + a - va
        return None

    def word(self, a):
        o = self.off(a)
        return None if o is None else struct.unpack_from("<I", self.d, o)[0]

    def cstr(self, a, maxlen=120):
        o = self.off(a)
        if o is None:
            return None
        e = self.d.find(b"\0", o, o + maxlen)
        if e < 0 or e - o < 2:
            return None
        s = self.d[o:e]
        if any((c < 0x20 or c > 0x7E) and c not in (9, 10) for c in s):
            return None
        return s.decode().replace("\n", "\\n")


class Listing:
    def __init__(self, path):
        self.funcs = {}     # addr -> (name, [insn])
        self.order = []
        self.calls = {}     # caller -> set(callee)
        self.callers = {}
        cur = None
        for line in open(path, errors="replace"):
            m = FUNC.match(line)
            if m:
                cur = int(m.group(1), 16)
                self.funcs[cur] = (m.group(2), [])
                self.order.append(cur)
                continue
            m = LINE.match(line.rstrip("\n"))
            if not m or cur is None or m.group(3).startswith("."):
                continue
            a = int(m.group(1), 16)
            ins = (a, m.group(3), m.group(4), m.group(5) or "", m.group(2) == "_")
            self.funcs[cur][1].append(ins)
            for c in re.findall(r"CALL ([0-9a-f]{8})", ins[3]):
                c = int(c, 16)
                self.calls.setdefault(cur, set()).add(c)
                self.callers.setdefault(c, set()).add(cur)
        self.names = {a: v[0] for a, v in self.funcs.items()}

    def name(self, a):
        return self.names.get(a, "%08x" % a)


BRANCH = re.compile(r"^(b|beq|bne|beql|bnel|blez|bgtz|bltz|bgez|blezl|bgtzl|bltzl|bgezl|"
                    r"bc1t|bc1f|bc1tl|bc1fl|bgezal|bltzal|j)$")


def simulate(insns, elf, stop_at=None):
    """Yield (insn, regs, stack) *before* each instruction executes."""
    targets = set()
    for a, mn, ops, refs, ds in insns:
        if BRANCH.match(mn):
            t = ops.split(",")[-1].strip()
            if t.startswith("0x"):
                targets.add(int(t, 16))
    regs = {"zero": 0, "gp": GP, "sp": ("sp", 0)}
    stack = {}
    pending_clobber = False
    for ins in insns:
        a, mn, ops, refs, ds = ins
        if a in targets:
            regs = {"zero": 0, "gp": GP, "sp": ("sp", 0)}
            # keep stack slots: frames are usually written once in straight-line setup
        yield ins, regs, stack
        do_step(ins, regs, stack, elf)
        if pending_clobber:          # the delay slot has executed: the call clobbers now
            for r in CALLER_SAVED:
                regs.pop(r, None)
            pending_clobber = False
        if mn in ("jal", "jalr"):
            pending_clobber = True


def do_step(ins, regs, stack, elf):
    a, mn, ops, refs, ds = ins
    o = [x.strip() for x in ops.split(",")] if ops else []
    try:
        step(mn, o, regs, stack, elf)
    except (ValueError, IndexError):
        if o:
            regs.pop(norm(o[0]), None)


MEM = re.compile(r"^(-?(?:0x)?[0-9a-f]+)\((\w+)\)$")


def val_add(v, k):
    if isinstance(v, int):
        return u32(v + k)
    if isinstance(v, tuple):
        return (v[0], v[1] + k)
    return None


def step(mn, o, regs, stack, elf):
    g = lambda r: regs.get(norm(r))
    def setr(r, v):
        r = norm(r)
        if r == "zero":
            return
        if v is None:
            regs.pop(r, None)
        else:
            regs[r] = v
    if mn == "lui":
        setr(o[0], u32(parse_int(o[1]) << 16))
    elif mn in ("addiu", "daddiu", "addi"):
        b = g(o[1])
        setr(o[0], val_add(b, parse_int(o[2])) if b is not None else None)
    elif mn == "ori":
        b = g(o[1])
        setr(o[0], u32(b | parse_int(o[2])) if isinstance(b, int) else None)
    elif mn == "li":
        setr(o[0], u32(parse_int(o[1])))
    elif mn == "move":
        setr(o[0], g(o[1]))
    elif mn in ("addu", "daddu", "or"):
        x, y = g(o[1]), g(o[2])
        if norm(o[2]) == "zero":
            setr(o[0], x)
        elif norm(o[1]) == "zero":
            setr(o[0], y)
        elif isinstance(x, int) and isinstance(y, int):
            setr(o[0], u32(x + y) if mn != "or" else u32(x | y))
        elif isinstance(x, tuple) and isinstance(y, int) and mn != "or":
            setr(o[0], val_add(x, y))
        else:
            setr(o[0], None)
    elif mn == "sll":
        x = g(o[1])
        setr(o[0], u32(x << parse_int(o[2])) if isinstance(x, int) else None)
    elif mn in ("sw", "sd", "sh", "sb", "sq"):
        m = MEM.match(o[1])
        if m:
            base = g(m.group(2))
            v = g(o[0])
            if isinstance(base, tuple) and base[0] == "sp":
                off = base[1] + parse_int(m.group(1))
                size = {"sw": 4, "sd": 8, "sh": 2, "sb": 1, "sq": 16}[mn]
                for i in range(off, off + size):
                    stack.pop(i, None)
                stack[off] = (v, size) if v is not None or norm(o[0]) == "zero" else ("?", size)
                if norm(o[0]) == "zero":
                    stack[off] = (0, size)
    elif mn in ("lw", "ld", "lh", "lhu", "lb", "lbu", "lq", "lwu"):
        m = MEM.match(o[1])
        v = None
        if m:
            base = g(m.group(2))
            off = parse_int(m.group(1))
            if isinstance(base, tuple) and base[0] == "sp":
                s = stack.get(base[1] + off)
                v = s[0] if s and s[0] != "?" else None
            elif isinstance(base, int) and mn == "lw":
                ad = u32(base + off)
                v = ("mem", ad)
        setr(o[0], v)
    elif o and not mn.startswith(("s", "b", "j", "c.", "mt", "nop", "sync", "ei", "di")):
        setr(o[0], None)


def fmt(v, elf, stack=None, deref=True):
    if v is None:
        return "?"
    if isinstance(v, tuple):
        if v[0] == "sp":
            s = ("sp+0x%x" % v[1]) if v[1] >= 0 else ("sp-0x%x" % -v[1])
            if deref and stack is not None:
                words = []
                for off in range(v[1], v[1] + 0x30, 4):
                    e = stack.get(off)
                    if e is None:
                        words.append("_")
                    else:
                        words.append(fmt(e[0], elf, None, False) if e[0] != "?" else "?")
                while words and words[-1] == "_":
                    words.pop()
                if words:
                    s += " [" + " ".join(words) + "]"
            return s
        if v[0] == "mem":
            ad = v[1]
            rel = " (gp%+#x)" % (ad - GP) if abs(ad - GP) < 0x8000 else ""
            w = elf.word(ad)
            return "*0x%x%s" % (ad, rel) + (" =init 0x%x" % w if w is not None else " (bss)")
    s = "0x%x" % v
    if deref and v > 0x100000:
        st = elf.cstr(v)
        if st:
            s += ' "%s"' % st
        if abs(v - GP) < 0x8000:
            s += " (gp%+#x)" % (v - GP)
    return s


def resolve_target(lst, t):
    t = t.strip()
    try:
        return int(t, 16)
    except ValueError:
        for a, n in lst.names.items():
            if n == t or n.endswith("::" + t):
                return a
    raise SystemExit("unknown target " + t)


def main():
    lst = Listing(sys.argv[1])
    elf = Elf(sys.argv[2])
    cmd = sys.argv[3]
    if cmd == "calls":
        targets = {resolve_target(lst, t) for t in sys.argv[4].split(",")}
        nargs = int(sys.argv[5]) if len(sys.argv) > 5 else 4
        for fa in lst.order:
            name, insns = lst.funcs[fa]
            sim = list_states(insns, elf)
            for i, (a, mn, ops, refs, ds) in enumerate(insns):
                cs = [int(c, 16) for c in re.findall(r"CALL ([0-9a-f]{8})", refs)]
                if not any(c in targets for c in cs) or mn not in ("jal", "j", "jalr"):
                    continue
                regs, stack = sim[i + 1]
                if i + 1 < len(insns):        # apply the delay slot
                    regs, stack = dict(regs), dict(stack)
                    do_step(insns[i + 1], regs, stack, elf)
                callee = [c for c in cs if c in targets][0]
                args = " ".join("%s=%s" % (r, fmt(regs.get(r), elf, stack)) for r in ARGS[:nargs])
                print("%08x in %s(%08x) -> %s: %s" % (a, name, fa, lst.name(callee), args))
    elif cmd == "codeptrs":
        import bisect
        starts = sorted(lst.funcs)
        text_end = int(sys.argv[4], 16) if len(sys.argv) > 4 else 0x42C640
        insn_addrs = {i[0] for f in lst.funcs.values() for i in f[1]}
        refs = {}
        for fa in lst.order:
            for a, mn, ops, r, ds in lst.funcs[fa][1]:
                if mn.startswith(("b", "j")):
                    continue
                for t in re.findall(r"-> ([0-9a-f]{8})", r):
                    t = int(t, 16)
                    if 0x100000 <= t < text_end and t % 4 == 0 and t not in lst.funcs:
                        refs.setdefault(t, set()).add(fa)
        for t in sorted(refs):
            owner = starts[bisect.bisect_right(starts, t) - 1]
            users = sorted(u for u in refs[t] if u != owner)
            if users:
                print("%08x code=%s near=%08x from %s" % (t, "yes" if t in insn_addrs else "no", owner,
                      " ".join("%08x" % u for u in users[:6])))
    elif cmd == "func":
        fa = resolve_target(lst, sys.argv[4])
        name, insns = lst.funcs[fa]
        print("==== %08x %s" % (fa, name))
        for a, mn, ops, refs, ds in insns:
            print("%08x  %s%s %s%s" % (a, "_" if ds else "", mn, ops, ("   ;" + refs) if refs else ""))
    elif cmd == "callers":
        fa = resolve_target(lst, sys.argv[4])
        for c in sorted(lst.callers.get(fa, ())):
            print("%08x %s" % (c, lst.name(c)))
    elif cmd == "tree":
        fa = resolve_target(lst, sys.argv[4])
        depth = int(sys.argv[5]) if len(sys.argv) > 5 else 3
        seen = set()
        def walk(f, d, ind):
            # callees in call order
            name, insns = lst.funcs.get(f, (lst.name(f), []))
            print("%s%08x %s%s" % ("  " * ind, f, name, " (seen)" if f in seen else ""))
            if f in seen or d == 0:
                return
            seen.add(f)
            order = []
            for a, mn, ops, refs, ds in insns:
                for c in re.findall(r"CALL ([0-9a-f]{8})", refs):
                    c = int(c, 16)
                    if c not in order:
                        order.append(c)
            for c in order:
                walk(c, d - 1, ind + 1)
        walk(fa, depth, 0)


def list_states(insns, elf):
    """State before each instruction index, plus one extra after the last."""
    out = []
    last = None
    for ins, regs, stack in simulate(insns, elf):
        out.append((dict(regs), dict(stack)))
    # state after final instruction is not tracked separately; reuse last
    if out:
        out.append(out[-1])
    return out


if __name__ == "__main__":
    main()
