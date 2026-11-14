"""Spike: translate every function of a 32-bit PE (Patrician3.exe) with the forked NFS recompiler and compile it for
arm64. Generic replacement for NFS build_all.py (no NFS patches, no fingerprint, bounds from the PE header).

    python p3_build.py --exe path/to/Patrician3.exe [--functions ../manifest/functions_ghidra.tsv] [--out ../out/build]
                       [--jobs N] [--no-compile]
"""
import argparse
import collections
import concurrent.futures
import os
import re
import struct
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import recomp  # noqa: E402
from capstone import CS_ARCH_X86, CS_MODE_32, Cs  # noqa: E402

CHUNK = 800


def sections(pe):
    out = {}
    for name, vs, va, rs, raw in pe.sections:
        out[name.rstrip(b"\0").decode(errors="replace")] = (pe.base + va, pe.base + va + max(vs, rs))
    return out


def linear_sweep(pe, lo, hi):
    """Instruction starts, and per instruction end: (start, kind) with kind pad/end/other (as NFS listing.py)."""
    text = pe.read(lo, hi - lo)
    md = Cs(CS_ARCH_X86, CS_MODE_32)
    starts, ends, imms = set(), {}, []
    off = 0
    while off < len(text):
        for a, n, mn, ops in md.disasm_lite(text[off:], lo + off):
            off = a + n - lo
            if mn in ("push", "mov"):
                m = re.match(r"^(?:.*, )?0x([0-9a-f]{6})$", ops)
                if m:
                    imms.append(int(m.group(1), 16))
            if mn in ("int3", "nop") or (mn == "mov" and ops == "edi, edi"):
                k = "pad"
            elif mn.startswith("ret") or mn in ("jmp", "hlt", "ud2", "iretd"):
                k = "end"
            else:
                k = "other"
            starts.add(a)
            ends[a + n] = (a, k)
        if off < len(text):
            off += 1
    return starts, ends, imms


def plausible_start(t, starts, ends):
    if t not in starts:
        return False
    at, k = t, 0
    while k < 32:
        prev = ends.get(at)
        if prev is None:
            return True
        a, kind = prev
        if kind == "pad":
            at, k = a, k + 1
            continue
        return kind == "end"
    return True


def load_functions(path, text_lo, text_hi):
    starts = {}
    for line in open(path):
        if line.startswith("#") or not line.strip():
            continue
        p = line.rstrip("\n").split("\t")
        a = int(p[0], 16)
        if text_lo <= a < text_hi:
            starts[a] = p[2]
    return starts


def add_data_referenced(pe, named, text_lo, text_hi, sec):
    starts, ends, imms = linear_sweep(pe, text_lo, text_hi)
    extra, rejected = set(), 0
    for name, (lo, hi) in sec.items():
        if lo == text_lo:
            continue
        blob = pe.read(lo, hi - lo) or b""
        for k in range(0, len(blob) - 3, 4):
            t = struct.unpack_from("<I", blob, k)[0]
            if text_lo <= t < text_hi and t not in named:
                prev = pe.read(t - 1, 1)
                before = pe.read(t - 5, 5) or b"\0" * 5
                after = before[0] == 0xE9 or before[3] == 0xEB or before[2] == 0xC2
                if t % 16 == 0 or prev in (b"\xcc", b"\xc3", b"\x90") or after:
                    if plausible_start(t, starts, ends):
                        extra.add(t)
                    else:
                        rejected += 1
    n_imm = 0
    for t in imms:
        if text_lo <= t < text_hi and t not in named and t not in extra:
            prev = pe.read(t - 1, 1)
            before = pe.read(t - 5, 5) or b"\0" * 5
            if t % 16 == 0 or prev in (b"\xcc", b"\xc3", b"\x90") or before[2] == 0xC2:
                if plausible_start(t, starts, ends):
                    extra.add(t)
                    n_imm += 1
                else:
                    rejected += 1
    return extra, n_imm, rejected


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", default=os.environ.get("P3_EXE"), help="Patrician3.exe (default: $P3_EXE)")
    ap.add_argument("--functions", default=os.path.join(HERE, "..", "manifest", "functions_ghidra.tsv"))
    ap.add_argument("--out", default=os.path.join(HERE, "..", "out", "build"))
    ap.add_argument("--jobs", type=int, default=os.cpu_count())
    ap.add_argument("--no-compile", action="store_true")
    args = ap.parse_args()
    if not args.exe:
        ap.error("pass --exe path/to/Patrician3.exe (or set P3_EXE)")
    os.makedirs(args.out, exist_ok=True)
    pe = recomp.PE(args.exe)
    sec = sections(pe)
    text_lo, text_hi = sec[".text"]
    named = load_functions(args.functions, text_lo, text_hi)
    extra, n_imm, rejected = add_data_referenced(pe, named, text_lo, text_hi, sec)
    allstarts = sorted([(a, n) for a, n in named.items()] + [(t, f"data_ref_{t:08X}") for t in extra])
    funcs = [(a, (allstarts[k + 1][0] if k + 1 < len(allstarts) else text_hi), n) for k, (a, n) in enumerate(allstarts)]
    print(f"ghidra functions in .text: {len(named)}; data/immediate-referenced starts added: {len(extra)} "
          f"(of which code immediates {n_imm}); rejected candidates: {rejected}")

    rc = recomp.Recompiler(pe=pe, functions={a: l for a, l, _ in funcs})
    rc.calls = True
    t0 = time.time()
    bodies, reasons, insns = [], collections.Counter(), 0
    partial_reasons = collections.Counter()
    stats = collections.Counter()
    imports_used = collections.Counter()
    for a, limit, name in funcs:
        try:
            code, why = rc.translate(a, limit)
        except Exception as e:  # translator assertion/bug: record and stub
            code, why = None, f"translator exception: {type(e).__name__}: {str(e)[:80]}"
        if code is None:
            reasons[why] += 1
            code = f"void f_{a:08X}(Cpu *c) {{ untranslated(c, 0x{a:08X}u); }} /* {name}: {why} */"
        else:
            insns += code.count("\n    /* ")
            for m in re.finditer(r"untranslated\(c, 0x[0-9A-F]+u\); return; /\* (.*?) \*/", code):
                partial_reasons[m.group(1)] += 1
            stats["dispatch (indirect call/jmp)"] += code.count("dispatch(c,")
            stats["switch tables"] += code.count("switch (")
            stats["fs: segment accesses (SEH/TLS)"] += code.count("c->fs_base")
            stats["x87 helper uses"] += len(re.findall(r"\bx87_", code))
            stats["falls into next function"] += code.count("falls into the next function")
            for m in re.finditer(r"host_import\(c, 0x[0-9A-F]+u\); /\* (\S+) \*/", code):
                imports_used[m.group(1)] += 1
        bodies.append((a, name, code))
    print(f"translated in {time.time() - t0:.1f}s")

    with open(os.path.join(args.out, "funcs.h"), "w") as h:
        h.write('#pragma once\n#include "runtime.h"\nvoid untranslated(Cpu *c, uint32_t addr);\n')
        h.writelines(f"void f_{a:08X}(Cpu *c);\n" for a, _, _ in bodies)
    with open(os.path.join(args.out, "table.c"), "w") as t:
        t.write('#include "funcs.h"\n')
        t.write("const uint32_t guest_func_addr[] = {" + ",".join(f"0x{a:08X}u" for a, _, _ in bodies) + "};\n")
        t.write("void (*const guest_func_ptr[])(Cpu *) = {" + ",".join(f"f_{a:08X}" for a, _, _ in bodies) + "};\n")
        t.write(f"const unsigned guest_func_count = {len(bodies)};\n")
    files = []
    for k in range(0, len(bodies), CHUNK):
        path = os.path.join(args.out, f"p3_{k // CHUNK:02d}.c")
        with open(path, "w") as f:
            f.write('#include "funcs.h"\n\n' + "\n\n".join(c for _, _, c in bodies[k:k + CHUNK]))
        files.append(path)
    files.append(os.path.join(args.out, "table.c"))

    errors, objs, ctime = [], [], 0.0
    if not args.no_compile:
        t0 = time.time()

        def compile_one(path):
            obj = path[:-2] + ".o"
            r = subprocess.run(["clang", "-arch", "arm64", "-c"] + recomp.CFLAGS + ["-I", HERE, "-I", args.out, path, "-o", obj],
                               capture_output=True, text=True)
            return path, obj, r.returncode, r.stderr[-3000:]
        with concurrent.futures.ThreadPoolExecutor(args.jobs) as ex:
            for path, obj, rcode, err in ex.map(compile_one, files):
                (objs.append(obj) if rcode == 0 else errors.append((path, err)))
        if not errors:
            subprocess.run(["ar", "rcs", os.path.join(args.out, "p3.a")] + objs, check=True)
        ctime = time.time() - t0

    stubbed = sum(reasons.values())
    rep = [f"functions: {len(funcs)} (ghidra {len(named)} + data-referenced {len(extra)})",
           f"translated: {len(funcs) - stubbed} ({100 * (len(funcs) - stubbed) / len(funcs):.2f} %), stubbed: {stubbed}",
           f"instructions translated: {insns}",
           f"instructions trapped inside translated functions: {sum(partial_reasons.values())}",
           f"C files: {len(files)}, compile errors: {len(errors)}, compile time: {ctime:.0f}s",
           "stub reasons:"] + [f"  {n:5d}  {w}" for w, n in reasons.most_common(25)]
    rep += ["in-function trap reasons:"] + [f"  {n:5d}  {w}" for w, n in partial_reasons.most_common(25)]
    rep += ["code stats:"] + [f"  {n:6d}  {w}" for w, n in stats.most_common()]
    by_dll = collections.Counter()
    for k, n in imports_used.items():
        by_dll[k.split("!")[0]] += n
    rep += ["import call sites by DLL:"] + [f"  {n:6d}  {w}" for w, n in by_dll.most_common()]
    rep += [f"distinct imports called: {len(imports_used)}"]
    for path, err in errors[:3]:
        rep.append(f"ERROR {path}:\n{err}")
    with open(os.path.join(args.out, "report.txt"), "w") as r:
        r.write("\n".join(rep) + "\n")
    with open(os.path.join(args.out, "imports_used.tsv"), "w") as r:
        r.writelines(f"{n}\t{k}\n" for k, n in imports_used.most_common())
    with open(os.path.join(args.out, "stubs.tsv"), "w") as r:
        for a, name, code in bodies:
            if code.startswith(f"void f_{a:08X}(Cpu *c) {{ untranslated"):
                r.write(f"{a:08X}\t{name}\t{code.split('/* ', 1)[1][:-3]}\n")
    print("\n".join(rep))


if __name__ == "__main__":
    main()
