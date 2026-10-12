"""Assign every function to a module (game / RenderWare / Sony lib / C runtime).

Inputs (from ClassifyFunctions.java): classified.csv, strings.csv, calls.csv.
Outputs: modules.csv (per function) and modules.md (summary).

Seeds come from referenced strings; unlabeled functions then inherit a label
when every labeled caller agrees, or when both address neighbours agree
(static linking keeps each object file contiguous).

    python3 -I tools/modules.py out/
"""
import csv
import re
import sys
from collections import Counter, defaultdict
from pathlib import Path

# Ordered: first match wins.
RULES = [
    ("sony:libmpeg", r"macroblock|picture|slice_|_slice|quantizer|BDEC|CSC handler|pack_header|"
                     r"chroma|motion_type|modion type|MPEG ERROR|work area|profile/level|vertical size|"
                     r"second field|intra && skip|image buffer needs"),
    ("renderware",   r"_rw[A-Z]|\bRw[A-Z]|\bRp[A-Z]|PS2All|PS2Manager|Im3D|sweOpen|_swe|circular|"
                     r"\.csl\b|ImmRender|Invalid combination type|Invalid projection type|"
                     r"Only rendering sub system|Vanilla(Atomic|Material|WorldSector)|AllInOne|"
                     r"CL_ATTRIB|state_chainBlock|DMA alloc block|Dma chain|Dma system|"
                     r"Heap resized|subrastering|creating raster|nonsensical limited memory"),
    ("libc",         r"vfprintf|^atan2f$|^Infinity$|assertion '%s' failed|^\(null\)$|0123456789abcdef"),
    ("libstdc++",    r"vector::insert|std::|Memory allocation failure"),
    ("sony:libkernl", r"^TTY:|# TLB|UDNL|libkernl|rom0:ROMVER|Can't read rom"),
    ("sony:libgraph", r"sceGs|_CHCR=|_TADR=|_MADR=|VIF1_STAT"),
    ("sony:libsdr",  r"sceSdRemote|PsIIlibsdr"),
    ("sony:libdma",  r"^libdma:"),
    ("sony:libpad",  r"^libpad:|libpad\.a"),
    ("sony:libmc",   r"libmc|mcman|mcserv"),
]
RULES = [(name, re.compile(rx)) for name, rx in RULES]
# Generic game strings. Requires prose-like text so stray pointers into data
# (e.g. animation-name tables) don't seed labels.
GAME = re.compile(r"[ \\%]|\.(cpp|CPP|dat|scm|ifp|IFP|ipl|IPL)\b")


def classify_string(text):
    for name, rx in RULES:
        if rx.search(text):
            return name
    if len(text) >= 8 and GAME.search(text):
        return "game"
    return None


def main(out):
    out = Path(out)
    funcs = {r["address"]: r for r in csv.DictReader(open(out / "classified.csv"))}
    order = sorted(funcs, key=lambda a: int(a, 16))

    votes = defaultdict(Counter)
    for r in csv.DictReader(open(out / "strings.csv")):
        name = classify_string(r["text"])
        if name and r["function"] in funcs:
            votes[r["function"]][name] += 1
    for a, f in funcs.items():
        for lbl in f["labels"].split():
            if lbl.startswith("sony:"):
                votes[a][lbl] += 5
            elif lbl.startswith("rw:"):
                votes[a]["renderware"] += 5

    module = {a: v.most_common(1)[0][0] for a, v in votes.items()}
    source = {a: "strings" for a in module}
    # Only the kernel library issues syscalls directly (game code goes through its stubs).
    for a, f in funcs.items():
        if f["syscalls"] and a not in module:
            module[a] = "sony:libkernl"
            source[a] = "syscall"

    callers = defaultdict(set)
    for r in csv.DictReader(open(out / "calls.csv")):
        if r["callee"] in funcs:
            callers[r["callee"]].add(r["caller"])

    def fill_neighbours():
        """Label functions sandwiched between two functions of the same module."""
        n = 0
        prev = None
        for i, a in enumerate(order):
            if a in module:
                prev = module[a]
                continue
            nxt = next((module[b] for b in order[i + 1:] if b in module), None)
            if prev is not None and prev == nxt:
                module[a] = prev
                source[a] = "neighbours"
                n += 1
        return n

    def fill_callers():
        """Label functions whose labeled callers all agree. Weaker than neighbours:
        library entry points are called from game code, so run it only after."""
        n = 0
        for a in order:
            if a in module:
                continue
            labels = {module[c] for c in callers[a] if c in module}
            if len(labels) == 1:
                module[a] = labels.pop()
                source[a] = "callers"
                n += 1
        return n

    while fill_neighbours() or fill_callers():
        pass

    with open(out / "modules.csv", "w", newline="") as fh:
        w = csv.writer(fh)
        w.writerow(["address", "size", "module", "source", "hw", "syscalls", "cop2", "mmi"])
        for a in order:
            f = funcs[a]
            w.writerow([a, f["size"], module.get(a, "unknown"), source.get(a, ""),
                        f["hw"], f["syscalls"], f["cop2"], f["mmi"]])

    stats = defaultdict(lambda: Counter())
    sys_by_mod = defaultdict(set)
    hw_by_mod = defaultdict(Counter)
    for a in order:
        f, m = funcs[a], module.get(a, "unknown")
        s = stats[m]
        s["funcs"] += 1
        s["bytes"] += int(f["size"])
        hw = [h for h in f["hw"].split() if h != "SPR"]
        s["hw"] += bool(hw)
        s["sys"] += bool(f["syscalls"])
        s["cop2"] += int(f["cop2"])
        s["mmi"] += int(f["mmi"])
        sys_by_mod[m].update(f["syscalls"].split())
        hw_by_mod[m].update(hw)

    total = sum(s["bytes"] for s in stats.values())
    lines = ["| Module | Functions | Code | Share | HW funcs | Syscall funcs | VU0 ops | MMI ops |",
             "|---|---:|---:|---:|---:|---:|---:|---:|"]
    for m, s in sorted(stats.items(), key=lambda kv: -kv[1]["bytes"]):
        lines.append(f"| {m} | {s['funcs']} | {s['bytes'] / 1024:.0f} KB | "
                     f"{100 * s['bytes'] / total:.1f}% | {s['hw']} | {s['sys']} | {s['cop2']} | {s['mmi']} |")
    lines += ["", "Hardware blocks touched per module:", ""]
    for m in sorted(hw_by_mod, key=lambda m: -sum(hw_by_mod[m].values())):
        if hw_by_mod[m]:
            lines.append(f"- **{m}**: " + ", ".join(f"{k} ({v})" for k, v in hw_by_mod[m].most_common()))
    allsys = sorted({int(x) for v in sys_by_mod.values() for x in v if x != "?"})
    lines += ["", f"Syscalls used ({len(allsys)}): " + " ".join(map(str, allsys))]
    src = Counter(source.get(a, "unlabeled") for a in order)
    lines += ["", "Label provenance: " + ", ".join(f"{k} {v}" for k, v in src.most_common())]
    (out / "modules.md").write_text("\n".join(lines) + "\n")
    print("\n".join(lines))


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else "out")
