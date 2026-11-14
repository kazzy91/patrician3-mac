# patrician3-mac

An experiment in running *Patrician III: Rise of the Hanse* (Ascaron, 2003) natively on Apple Silicon. It uses
the static-recompilation approach of [nfs-most-wanted-mac](https://github.com/elforeign/nfs-most-wanted-mac):
the game's own 32-bit x86 code is translated to C ahead of time and compiled for arm64. Native host code then
replaces Win32 / DirectDraw / Miles. There is no Wine, CrossOver or emulator involved.

**Status: feasibility spike.** All of `Patrician3.exe` translates and compiles for arm64, but nothing runs yet:
there is no runtime, no host layer and no renderer. See [docs/SPIKE.md](docs/SPIKE.md) for the numbers and the road ahead.

You need your own copy of the game. This repository contains no game code, data or generated C. It holds only the
tooling and a manifest of function addresses.

## Layout

| Path | What |
|---|---|
| `recomp/recomp.py` | x86-32 → C translator, forked from nfs-most-wanted-mac (NFS-only patches disabled) |
| `recomp/runtime.h` | `Cpu` struct, guest-memory macros, flags and x87 helpers used by the generated C |
| `recomp/p3_build.py` | Translates every function of `Patrician3.exe` and compiles them → `out/build/p3.a` + `report.txt` |
| `manifest/functions_ghidra.tsv` | Function starts from Ghidra's analysis (addresses and names only) |
| `ghidra/ExportFunctions.java` | Ghidra post-script that writes the manifest |
| `scripts/` | `setup.sh` (Python venv), `build.sh`, `ghidra_export.sh`, `fork_recomp.py` (how the fork was made) |

## Build the spike

Requires: Xcode Command Line Tools (clang), Python 3, and optionally Ghidra (`brew install ghidra`) to regenerate the manifest.

```bash
scripts/setup.sh
```

```bash
scripts/build.sh "/path/to/Patrician III/Patrician3.exe"
```

Output goes to `out/build/` (`p3.a`, `report.txt`, `imports_used.tsv`, `stubs.tsv`).

To regenerate the function manifest (a few minutes):

```bash
scripts/ghidra_export.sh "/path/to/Patrician III/Patrician3.exe"
```

## Credits

The translator and runtime header come from nfs-most-wanted-mac (MIT); see [NOTICE](NOTICE).
