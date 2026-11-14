# patrician3-mac

An experiment in running *Patrician III: Rise of the Hanse* (Ascaron, 2003) natively on Apple Silicon. It uses
the static-recompilation approach of [nfs-most-wanted-mac](https://github.com/elforeign/nfs-most-wanted-mac):
the game's own 32-bit x86 code is translated to C ahead of time and compiled for arm64. Native host code then
replaces Win32 / DirectDraw / Miles. There is no Wine, CrossOver or emulator involved.

**Status: early boot.** `Patrician3.exe` and `Archiver.dll` translate fully and compile for arm64. The boot runtime
runs the game's startup natively, from the C runtime and MFC through the archive layer and the hardware check, up to
loading its renderer (`ddraw_dll.dll`), which doesn't exist yet. There is no window or graphics. See
[docs/SPIKE.md](docs/SPIKE.md) for translation and test results and [docs/BOOT.md](docs/BOOT.md) for the runtime.

You need your own copy of the game. This repository contains no game code, data or generated C. It holds only the
tooling and a manifest of function addresses.

## Layout

| Path | What |
|---|---|
| `recomp/recomp.py` | x86-32 → C translator, forked from nfs-most-wanted-mac (NFS-only patches disabled) |
| `recomp/runtime.h` | `Cpu` struct, guest-memory macros, flags and x87 helpers used by the generated C |
| `recomp/p3_build.py` | Translates every function of a module and compiles them → `out/build/<module>/<module>.a` + `report.txt` |
| `recomp/difftest.py` | Differential tests: translated C vs the original x86 code under Unicorn, on random states |
| `manifest/functions_ghidra.tsv`, `manifest/<module>_functions.tsv` | Function starts from Ghidra's analysis (addresses and names only) |
| `runtime/` | Boot runtime: loader, dispatch, and the Win32 subset (KERNEL32, USER32/GDI32 headless, registry, files) |
| `ghidra/ExportFunctions.java` | Ghidra post-script that writes the manifest |
| `scripts/` | `setup.sh` (Python venv), `build.sh`, `ghidra_export.sh`, `fork_recomp.py` (how the fork was made) |

## Build the spike

Requires: Xcode Command Line Tools (clang), Python 3, and optionally Ghidra (`brew install ghidra`) to regenerate the manifest.

```bash
scripts/setup.sh
```

```bash
scripts/build.sh "/path/to/Patrician III"
```

Output goes to `out/build/<module>/` (`<module>.a`, `report.txt`, `imports_used.tsv`, `stubs.tsv`). To link and run the boot:

```bash
runtime/build.sh
```

```bash
out/boot "/path/to/Patrician III"
```

Differential tests (leaf functions; add `--calls` for non-leaf functions with their callees compiled in):

```bash
.venv/bin/python recomp/difftest.py --exe "/path/to/Patrician III/Patrician3.exe" --limit 0 --tests 8 --log out/difftest_leaf.log
```

To regenerate a function manifest (a few minutes each):

```bash
scripts/ghidra_export.sh "/path/to/Patrician III/Patrician3.exe"
```

```bash
scripts/ghidra_export.sh "/path/to/Patrician III/archiver.dll" archiver
```

## Credits

The translator and runtime header come from nfs-most-wanted-mac (MIT); see [NOTICE](NOTICE).
