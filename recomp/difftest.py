"""Differential test: translated C vs the original x86 code of Patrician3.exe under Unicorn, on identical random states.

    python difftest.py --exe path/to/Patrician3.exe [--limit N] [--tests K] [--seed S] [--calls] [--only ADDR ...]

Adapted from nfs-most-wanted-mac recomp/difftest.py (MIT). The upstream harness needs a developer-only emulator
module (speed_emu); this one sets Unicorn up directly.

1. Build the function list exactly like p3_build.py (Ghidra manifest + data-referenced starts + call targets).
2. Leaf mode (default): translate with calls disabled, so only functions that call nothing translate. --calls: non-leaf
   roots whose callees (transitively, at most --max-closure functions) all translate are compiled in together.
3. Compile a sample into a dylib (guest memory = 4 GB anonymous reservation).
4. Per function, K random states: registers point into a random scratch area (or hold small values), the stack holds
   random arguments. The original runs under Unicorn first; states where it faults, raises a non-int3 interrupt or runs
   more than 2M instructions are skipped. The C version runs in a forked child, so a crash or hang can't take the
   run down. Compared: eax..edi, ST(0), and every byte of scratch, the stack window and .data.

Functions that use fs: (SEH frames, TLS) are left out: Unicorn needs a GDT for a 32-bit fs base, which this harness
doesn't set up yet. Their count is reported.
"""
import argparse
import ctypes
import math
import os
import random
import select
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import recomp  # noqa: E402
import p3_build  # noqa: E402

SCRATCH, SCRATCH_SIZE = 0x20000000, 0x10000
STACK_HI = 0x7FF00000
STACK_LO = STACK_HI - 0x100000
RETURN_TRAP = 0x7FFF0000
REGS = ["eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi"]


class CpuStruct(ctypes.Structure):     # must match struct Cpu in runtime.h
    _fields_ = [(r, ctypes.c_uint32) for r in REGS] + [
        ("fs_base", ctypes.c_uint32), ("st", ctypes.c_double * 8), ("top", ctypes.c_uint32),
        ("sw", ctypes.c_uint16), ("cw", ctypes.c_uint16), ("mm", ctypes.c_uint64 * 8),
        ("df", ctypes.c_uint32), ("mxcsr", ctypes.c_uint32), ("abi_skip", ctypes.c_uint32),
        ("eflags_ext", ctypes.c_uint32), ("eflags", ctypes.c_uint32)]


def fp80(mant, exp):
    if exp & 0x7FFF == 0 and mant == 0:
        return -0.0 if exp & 0x8000 else 0.0
    sign = -1.0 if exp & 0x8000 else 1.0
    if exp & 0x7FFF == 0x7FFF:
        return float("nan") if mant & ((1 << 63) - 1) else sign * float("inf")
    try:
        return sign * math.ldexp(mant / (1 << 63), (exp & 0x7FFF) - 16383)
    except OverflowError:
        return sign * float("inf")


def all_functions(pe):
    sec = p3_build.sections(pe)
    text_lo, text_hi = sec[".text"]
    named = p3_build.load_functions(os.path.join(HERE, "..", "manifest", "functions_ghidra.tsv"), text_lo, text_hi)
    extra, _, _ = p3_build.add_data_referenced(pe, named, text_lo, text_hi, sec)
    starts = dict(named)
    starts.update((t, f"data_ref_{t:08X}") for t in extra)
    p3_build.add_call_targets(pe, starts, text_lo, text_hi)
    return p3_build.make_funcs(starts, text_hi)


def build(rc, funcs, workdir):
    src = ['#include "runtime.h"', "uint8_t *G;", "void untranslated(Cpu *c, uint32_t at);"]
    src.append("\n".join(f"void f_{a:08X}(Cpu *c);" for a, _, _ in funcs))
    names = []
    for a, limit, _ in funcs:
        code, why = rc.translate(a, limit)
        if code is None:
            raise RuntimeError(f"{a:08x}: {why}")
        src.append(code)
        names.append(f"f_{a:08X}")
    src.append("typedef void (*Fn)(Cpu *);")
    src.append("static Fn table[] = {" + ", ".join(names) + "};")
    src.append("static const uint32_t addrs[] = {" + ", ".join(f"0x{a:08X}u" for a, _, _ in funcs) + "};")
    src.append('#include <setjmp.h>\n#include <sys/mman.h>\n'
               'static jmp_buf escape;\n'
               'uint8_t *guest_init(void) { G = mmap(0, 1ull << 32, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0); return G; }\n'
               'void dispatch(Cpu *c, uint32_t t) { int lo = 0, hi = (int)(sizeof addrs / 4) - 1;\n'
               '  while (lo <= hi) { int m = (lo + hi) / 2; if (addrs[m] == t) { table[m](c); return; } if (addrs[m] < t) lo = m + 1; else hi = m - 1; }\n'
               '  longjmp(escape, 2); }\n'
               'void guest_divide_error(Cpu *c, uint32_t at) { (void)c; (void)at; longjmp(escape, 6); }\n'
               'void guest_breakpoint(Cpu *c, uint32_t at) { (void)c; (void)at; }\n'
               'void untranslated(Cpu *c, uint32_t at) { (void)c; (void)at; longjmp(escape, 7); }\n'
               'void bad_jump(Cpu *c, uint32_t at) { (void)c; (void)at; longjmp(escape, 3); }\n'
               'void host_import(Cpu *c, uint32_t slot) { (void)c; (void)slot; longjmp(escape, 4); }\n'
               'uint64_t host_ticks(void) { return 0; }\n'
               'void host_cpuid(Cpu *c) { (void)c; longjmp(escape, 5); }\n'
               'int run(int i, Cpu *c) { int r = setjmp(escape); if (r) return r; table[i](c); return 0; }')
    cfile = os.path.join(workdir, "gen.c")
    open(cfile, "w").write("\n\n".join(src))
    lib = os.path.join(workdir, "gen.dylib")
    subprocess.run(["clang", "-arch", "arm64", "-shared", "-fPIC"] + recomp.CFLAGS + ["-I", HERE, cfile, "-o", lib], check=True)
    return lib, cfile


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", default=os.environ.get("P3_EXE"), help="Patrician3.exe (default: $P3_EXE)")
    ap.add_argument("--limit", type=int, default=2000, help="functions to sample (0 = all)")
    ap.add_argument("--tests", type=int, default=8, help="random states per function")
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--only", nargs="*", default=[], help="hex addresses to test")
    ap.add_argument("--calls", action="store_true", help="test non-leaf functions (callees compiled in)")
    ap.add_argument("--max-closure", type=int, default=40)
    ap.add_argument("--log", help="also write the report to this file")
    args = ap.parse_args()
    if not args.exe:
        ap.error("pass --exe path/to/Patrician3.exe (or set P3_EXE)")
    from unicorn import UC_ARCH_X86, UC_HOOK_INTR, UC_MODE_32, Uc, UcError
    from unicorn.x86_const import (UC_X86_REG_EAX, UC_X86_REG_EBP, UC_X86_REG_EBX, UC_X86_REG_ECX, UC_X86_REG_EDI,
                                   UC_X86_REG_EDX, UC_X86_REG_EFLAGS, UC_X86_REG_EIP, UC_X86_REG_ESI, UC_X86_REG_ESP,
                                   UC_X86_REG_FP0, UC_X86_REG_FPCW, UC_X86_REG_FPSW, UC_X86_REG_FPTAG)
    UREG = dict(zip(REGS, [UC_X86_REG_EAX, UC_X86_REG_ECX, UC_X86_REG_EDX, UC_X86_REG_EBX, UC_X86_REG_ESP,
                           UC_X86_REG_EBP, UC_X86_REG_ESI, UC_X86_REG_EDI]))
    rnd = random.Random(args.seed)
    pe = recomp.PE(args.exe)
    funcs = all_functions(pe)
    rc = recomp.Recompiler(pe=pe, functions={a: l for a, l, _ in funcs})
    rc.calls = args.calls
    ok, callees, reasons, uses_fs = [], {}, {}, 0
    for a, limit, name in funcs:
        code, why = rc.translate(a, limit)
        if code is None:
            reasons[why] = reasons.get(why, 0) + 1
        elif "c->fs_base" in code:
            uses_fs += 1
        else:
            ok.append((a, limit, name))
            callees[a] = set(rc.callees)
    out_lines = []

    def say(s):
        print(s)
        out_lines.append(s)
    mode = "with calls" if args.calls else "leaf only"
    say(f"translatable ({mode}): {len(ok)} of {len(funcs)} functions; left out because they use fs: {uses_fs}")
    for why, n in sorted(reasons.items(), key=lambda t: -t[1])[:8]:
        say(f"  {n:6d}  {why}")
    if args.only:
        want = {int(x, 16) for x in args.only}
        ok = [f for f in ok if f[0] in want]
    sample = ok if not args.limit or len(ok) <= args.limit else rnd.sample(ok, args.limit)
    sample.sort()
    if args.calls:
        good = {f[0] for f in ok}
        info = {f[0]: f for f in ok}
        roots, members = [], set()
        for f in sample:
            seen, stack_, bad = set(), [f[0]], False
            while stack_ and not bad:
                x = stack_.pop()
                if x in seen:
                    continue
                if x not in good or len(seen) > args.max_closure:
                    bad = True
                    break
                seen.add(x)
                stack_.extend(callees[x])
            if not bad and len(seen) > 1:
                roots.append(f)
                members |= seen
        say(f"non-leaf roots with a translatable closure of <= {args.max_closure}: {len(roots)} ({len(members)} functions compiled)")
        compile_set = sorted(info[x] for x in members)
        sample = roots
    else:
        compile_set = sample
    index_of = {f[0]: k for k, f in enumerate(compile_set)}
    work = tempfile.mkdtemp(prefix="p3diff_")
    lib_path, cfile = build(rc, compile_set, work)
    lib = ctypes.CDLL(lib_path)
    lib.guest_init.restype = ctypes.POINTER(ctypes.c_uint8)
    gbase = ctypes.addressof(lib.guest_init().contents)

    image = [(pe.base + va, pe.data[raw:raw + rs]) for _, vs, va, rs, raw in pe.sections]
    data_sec = [(pe.base + va, max(vs, rs)) for nm, vs, va, rs, raw in pe.sections if nm.startswith(b".data")][0]
    data_init = [(pe.base + va, (pe.data[raw:raw + rs] + bytes(max(vs, rs)))[:max(vs, rs)])
                 for nm, vs, va, rs, raw in pe.sections if nm.startswith(b".data")][0]
    img_lo = pe.base
    img_hi = max(pe.base + va + max(vs, rs) for _, vs, va, rs, _ in pe.sections)

    def guest_write(a, b):
        ctypes.memmove(gbase + a, b, len(b))

    def guest_read(a, n):
        return ctypes.string_at(gbase + a, n)
    for va, b in image:
        guest_write(va, b)

    uc = Uc(UC_ARCH_X86, UC_MODE_32)
    uc.mem_map(img_lo, (img_hi - img_lo + 0xFFF) & ~0xFFF)
    for va, b in image:
        uc.mem_write(va, b)
    uc.mem_map(SCRATCH, SCRATCH_SIZE)
    uc.mem_map(STACK_LO, STACK_HI - STACK_LO)
    uc.mem_map(RETURN_TRAP, 0x1000)
    uc.mem_write(RETURN_TRAP, b"\xF4")
    other_intr = []

    def on_intr(u, intno, _):
        if intno != 3:          # int3 continues (the port's model); anything else ends the run as skipped
            other_intr.append(intno)
            u.emu_stop()
    uc.hook_add(UC_HOOK_INTR, on_intr)

    passed = failed = skipped = crashed = 0
    fail_list, no_verdict = [], []
    for a, _, name in sample:
        idx = index_of[a]
        verdicts = []
        for _ in range(args.tests):
            scratch = bytes(rnd.getrandbits(8) for _ in range(256)) * (SCRATCH_SIZE // 256)
            stack = bytes(rnd.getrandbits(8) for _ in range(0x800))
            regs = {r: SCRATCH + rnd.randrange(0x1000, 0xF000, 4) if rnd.random() < 0.7 else rnd.getrandbits(8)
                    for r in ["eax", "ecx", "edx", "ebx", "ebp", "esi", "edi"]}
            esp = STACK_HI - 0x800
            args_ = [SCRATCH + rnd.randrange(0x1000, 0xF000, 4) if rnd.random() < 0.6 else rnd.getrandbits(8) for _ in range(8)]
            stack_img = struct.pack("<I", RETURN_TRAP) + struct.pack("<8I", *args_)
            other_intr.clear()
            uc.mem_write(*data_init)
            uc.mem_write(SCRATCH, scratch)
            uc.mem_write(esp - 0x400, stack)
            uc.mem_write(esp, stack_img)
            for r, v in regs.items():
                uc.reg_write(UREG[r], v)
            uc.reg_write(UC_X86_REG_ESP, esp)
            uc.reg_write(UC_X86_REG_EFLAGS, 0x202)
            uc.reg_write(UC_X86_REG_FPCW, 0x027F)
            uc.reg_write(UC_X86_REG_FPSW, 0)
            uc.reg_write(UC_X86_REG_FPTAG, 0xFFFF)
            try:
                uc.emu_start(a, RETURN_TRAP, count=2_000_000)
            except UcError:
                skipped += 1
                continue
            if other_intr or uc.reg_read(UC_X86_REG_EIP) != RETURN_TRAP:
                skipped += 1
                continue
            want = {r: uc.reg_read(UREG[r]) for r in REGS}
            if not STACK_LO <= want["esp"] <= STACK_HI:
                skipped += 1
                continue
            w_top = (uc.reg_read(UC_X86_REG_FPSW) >> 11) & 7          # Unicorn's FP0..7 are physical registers
            w_st0 = fp80(*uc.reg_read(UC_X86_REG_FP0 + w_top)) if w_top != 0 else None
            w_scr = bytes(uc.mem_read(SCRATCH, SCRATCH_SIZE))
            w_stk = bytes(uc.mem_read(esp - 0x400, 0x800))
            w_dat = bytes(uc.mem_read(*data_sec))
            rfd, wfd = os.pipe()
            pid = os.fork()
            if pid == 0:
                os.close(rfd)
                guest_write(SCRATCH, scratch)
                guest_write(esp - 0x400, stack)
                guest_write(esp, stack_img)
                c = CpuStruct()
                for r, v in regs.items():
                    setattr(c, r, v)
                c.esp, c.cw, c.top = esp, 0x027F, 0
                if lib.run(idx, ctypes.byref(c)) != 0:
                    os.write(wfd, b"SKIP")          # reached an import, an unknown dispatch target or a #DE
                    os._exit(0)
                got = {r: getattr(c, r) for r in REGS}
                msg = [f"{r} {got[r]:#x} != {want[r]:#x}" for r in REGS if got[r] != want[r]]
                g_st0 = c.st[c.top & 7] if c.top != 0 else None
                if (w_st0 is None) != (g_st0 is None) or (w_st0 is not None and not (
                        w_st0 == g_st0 or (w_st0 != w_st0 and g_st0 != g_st0) or abs(w_st0 - g_st0) <= 1e-9 * max(1, abs(w_st0)))):
                    msg.append(f"st0 {g_st0} != {w_st0}")
                for nm_, base_, want_b in (("scratch", SCRATCH, w_scr), ("stack", esp - 0x400, w_stk), (".data", data_sec[0], w_dat)):
                    got_b = guest_read(base_, len(want_b))
                    if got_b != want_b:
                        k = next(j for j in range(len(want_b)) if got_b[j] != want_b[j])
                        k4 = k & ~3
                        nd = sum(1 for j in range(len(want_b)) if got_b[j] != want_b[j])
                        msg.append(f"{nm_} differs at {base_ + k:#x} ({nd} bytes): got {got_b[k4:k4 + 4].hex()} want {want_b[k4:k4 + 4].hex()}")
                os.write(wfd, ("; ".join(msg) or "OK").encode()[:4000])
                os._exit(0)
            os.close(wfd)
            r_, _, _ = select.select([rfd], [], [], 10)
            out = os.read(rfd, 4096).decode() if r_ else "TIMEOUT"
            os.close(rfd)
            if not r_:
                os.kill(pid, 9)
            _, status = os.waitpid(pid, 0)
            verdicts.append(out or f"CRASH status {status}")
        verdicts = [v for v in verdicts if v != "SKIP"]
        if not verdicts:
            no_verdict.append(name)
        elif all(v == "OK" for v in verdicts):
            passed += 1
        else:
            bad = next(v for v in verdicts if v != "OK")
            crashed += bad.startswith(("CRASH", "TIMEOUT"))
            failed += 1
            fail_list.append((a, name, bad))
    say(f"differential test ({mode}, {args.tests} states each, seed {args.seed}): {len(sample)} functions sampled, "
        f"{passed} pass, {failed} fail ({crashed} crash/timeout), {len(no_verdict)} no verdict; "
        f"states skipped because the original faulted: {skipped}")
    for a, name, why in fail_list[:60]:
        say(f"  FAIL {a:08x} {name}: {why[:220]}")
    say(f"generated C: {cfile}")
    if args.log:
        open(args.log, "w").write("\n".join(out_lines) + "\n")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
