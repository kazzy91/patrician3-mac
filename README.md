# patrician3-mac

An experiment in running *Patrician III: Rise of the Hanse* (Ascaron, 2003) natively on Apple Silicon. It uses
the static-recompilation approach of [nfs-most-wanted-mac](https://github.com/elforeign/nfs-most-wanted-mac):
the game's own 32-bit x86 code is translated to C ahead of time and compiled for arm64. Native host code then
replaces Win32 / DirectDraw / Miles. There is no Wine, CrossOver or emulator involved.

**Status: playable.** `Patrician3.exe` and five of its DLLs are recompiled to arm64. The game runs natively in a
Mac window at 1280x1024 with sound: menus, towns, the trading screens and moving between towns work in hand play.
See [docs/BOOT.md](docs/BOOT.md) and [docs/SPIKE.md](docs/SPIKE.md).

## Install

1. Have Patrician III installed somewhere on the Mac, for example in a CrossOver bottle. Setup supports the English
   retail CD version (it checks the files' hashes, `manifest/hashes.tsv`).
2. Open `Patrician-III-Setup-<version>.dmg` and run **Patrician III Setup**. The first time, macOS may block it
   because it isn't notarized: open System Settings > Privacy & Security and click **Open Anyway**.
3. Setup finds a CrossOver install by itself, or click **Choose Folder…**. Click **Build and Install**. Setup checks
   your files, translates and compiles the game on this Mac (about 2 minutes, with Apple's Command Line Tools; Setup
   offers to install them), and puts **Patrician III.app** in Applications. The app holds its own copy of the game
   files, so it doesn't need CrossOver afterwards.

Saves and settings: `~/Library/Application Support/patrician3-mac`. Log: `~/Library/Logs/Patrician III.log`.
Apple silicon only.

Make the disk image with `scripts/make_dmg.sh` (→ `out/Patrician-III-Setup-<version>.dmg`).

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
| `runtime/` | Runtime: loader, dispatch, Win32 (KERNEL32, USER32/GDI32, registry, files), DirectDraw, Miles (CoreAudio), the Mac window and input |
| `manifest/bases.tsv` | Load addresses for DLLs that share a base with another module |
| `ghidra/ExportFunctions.java` | Ghidra post-script that writes the manifest |
| `scripts/` | `setup.sh` (Python venv), `build.sh`, `app.sh` (the .app), `icon.py` (the game's icon), `make_dmg.sh`, `ghidra_export.sh`, `fork_recomp.py` (how the fork was made) |
| `setup/` | The Setup app (`Setup.m`) and the build it runs (`install.sh`) |
| `manifest/hashes.tsv` | SHA-256 of the supported game files |

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
