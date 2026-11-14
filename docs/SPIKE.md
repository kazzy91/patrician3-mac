# Feasibility spike (2026-11-14)

Question: can the nfs-most-wanted-mac static-recompilation approach port Patrician III to macOS natively?

## Input

`Patrician3.exe`: English v1.x, internal name `Patrician3_unprotected.exe`, no DRM. 32-bit PE, MSVC 6, with MFC
and the C runtime statically linked. Image base 0x400000, `.text` 0x2682C5 bytes.
SHA-256 `1f4012acf23f81b72f81506f46cc83967ff207af0de3025323750f9d2d676707`.

## Result

| | Patrician III | NFS Most Wanted (upstream, for scale) |
|---|---|---|
| Functions | 12,050 (Ghidra 9,210 + 2,840 code pointers found in data/immediates) | 52,656 |
| Translated | **12,050 (100 %)**, 718,007 instructions | 52,458 (99.6 %) |
| Trapped instructions inside functions | 36 (all "call to unknown target") | MMX in ~198 stub functions |
| arm64 compile | 17 C files, **0 errors**, 22 s; `p3.a` 10 MB (7.5 MB `__text`) | 69 files, 0 errors |

Of the Ghidra functions, 3,725 are SEH `Unwind@` funclets and 34 are `Catch@` blocks.

What this does **not** show: correctness (no differential tests have run yet) or that anything boots.

## Work that remains

1. **36 unknown call targets.** Calls such as `call 0x4B92E0` hit addresses that Ghidra didn't mark as function
   starts. Fix: add every direct-call target to the function list in `p3_build.py`.
2. **Differential tests.** Port `difftest.py` from upstream (Unicorn vs. translated C) and run it on the leaf functions.
3. **Runtime / boot.** Upstream's `runtime/` provides the guest memory, PE loader, import thunks, heap, threads and SEH.
   3,479 `fs:` accesses mean SEH/TLS has to work from the start. 10,402 indirect `dispatch()` sites.
4. **Host layer: 333 distinct imports are called.**
   - USER32: 514 call sites, mostly window and rect helpers plus dialogs and the message loop. This is the biggest
     gap: upstream targets a full-screen D3D9 game and has little GUI coverage.
   - KERNEL32: 424 call sites.
   - mss32 (Miles Sound System): 147 sites / 43 functions → CoreAudio.
   - SchnellTransfer (LAN multiplayer): 45 sites / 18 functions → BSD sockets, or stub.
   - VTO (video): 8 sites / 6 functions → FFmpeg (`.bik` files).
   - GDI32: 19 sites.
5. **Renderer.** The game loads `ddraw_dll.dll` with `LoadLibrary`/`GetProcAddress` and calls 198 `sgl_*` exports
   (Ascaron's own "SGL" engine on DirectDraw / D3D7) through function pointers. Plan: implement the SGL API
   directly on Metal, and have `dispatch()` send the `GetProcAddress` results to host functions. That avoids
   emulating DirectDraw and D3D7.
6. **Helper DLLs.** `Archiver.dll` (the `.cpr` archive), `AIM.dll` and `Vto.dll` must be either rewritten or recompiled.
   AIM, Archiver, Vto and SchnellTransfer all have image base 0x10000000, so recompiling them means relocating
   them at translate time.
