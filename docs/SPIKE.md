# Feasibility spike (2026-11-14)

Question: can the nfs-most-wanted-mac static-recompilation approach port Patrician III to macOS natively?

## Input

`Patrician3.exe`: English v1.x, internal name `Patrician3_unprotected.exe`, no DRM. 32-bit PE, MSVC 6, with MFC
and the C runtime statically linked. Image base 0x400000, `.text` 0x2682C5 bytes.
SHA-256 `1f4012acf23f81b72f81506f46cc83967ff207af0de3025323750f9d2d676707`.

## Result

| | Patrician III | NFS Most Wanted (upstream, for scale) |
|---|---|---|
| Functions | 12,074 (Ghidra 9,210 + 2,840 code pointers found in data/immediates + 24 direct-call targets Ghidra missed) | 52,656 |
| Translated | **12,074 (100 %)**, 718,813 instructions | 52,458 (99.6 %) |
| Trapped instructions inside functions | 0 (the first run had 36 "call to unknown target"; fixed by adding call targets) | MMX in ~198 stub functions |
| arm64 compile | 17 C files, **0 errors**, 22 s; `p3.a` 10 MB (7.5 MB `__text`) | 69 files, 0 errors |

Of the Ghidra functions, 3,725 are SEH `Unwind@` funclets and 34 are `Catch@` blocks.

What this does **not** show: that anything boots.

## Differential tests

`recomp/difftest.py` runs each function's original x86 code under Unicorn and its translated C natively, from the
same random states, then compares registers, ST(0), scratch memory, the stack window and `.data`. Logs: `out/difftest_*.log`.

| Run (8 states per function, seed 1) | Sampled | Pass | Fail | No verdict |
|---|---|---|---|---|
| All leaf functions (functions that call nothing) | 1,142 | **947** | **0** | 195 |
| Non-leaf, 3,000 sampled; roots whose callee closure (≤ 40 functions) translates and avoids `fs:` | 425 (808 compiled) | **131** | **0** | 294 |

"No verdict" means no state reached the end in both versions. Either the original faulted on a random pointer (most
of them), or the C side reached a Win32 import or an indirect call to a function outside the compiled set.

Coverage limits:
- Only 1,142 functions are leaves. 4,053 functions end in a tail jump (mostly SEH `Unwind@` funclets that jump to
  `__CxxFrameHandler`), and 6,615 make calls.
- Functions that touch `fs:` (1,188 of the non-leaf ones) are excluded, because the harness has no GDT for a 32-bit fs base yet.
- Unicorn is the reference, not a real CPU.

## Work that remains

1. **Wider differential coverage.** Give Unicorn a GDT so SEH/`fs:` functions can be tested. Stub imports on both
   sides, so that functions calling Win32 helpers reach a verdict.
2. **Runtime / boot.** Started: see [BOOT.md](BOOT.md). The rest of this item is the original estimate. Upstream's `runtime/` provides the guest memory, PE loader, import thunks, heap, threads and SEH.
   3,479 `fs:` accesses mean SEH/TLS has to work from the start. 10,424 indirect `dispatch()` sites.
3. **Host layer: 333 distinct imports are called.**
   - USER32: 514 call sites, mostly window and rect helpers plus dialogs and the message loop. This is the biggest
     gap: upstream targets a full-screen D3D9 game and has little GUI coverage.
   - KERNEL32: 424 call sites.
   - mss32 (Miles Sound System): 147 sites / 43 functions → CoreAudio.
   - SchnellTransfer (LAN multiplayer): 45 sites / 18 functions → BSD sockets, or stub.
   - VTO (video): 8 sites / 6 functions → FFmpeg (`.bik` files).
   - GDI32: 19 sites.
4. **Renderer.** The game loads `ddraw_dll.dll` with `LoadLibrary`/`GetProcAddress` and calls 198 `sgl_*` exports
   (Ascaron's own "SGL" engine on DirectDraw / D3D7) through function pointers. Plan: implement the SGL API
   directly on Metal, and have `dispatch()` send the `GetProcAddress` results to host functions. That avoids
   emulating DirectDraw and D3D7.
5. **Helper DLLs.** `Archiver.dll` (the `.cpr` archive), `AIM.dll` and `Vto.dll` must be either rewritten or recompiled.
   AIM, Archiver, Vto and SchnellTransfer all have image base 0x10000000, so recompiling them means relocating
   them at translate time.
