# Boot runtime (2026-11-14)

`runtime/` links the translated modules into one arm64 program, `out/boot`, and runs the game natively in a Mac
window. Every Windows, DirectDraw and Miles call the game makes goes to the host layer in `runtime/`. The first
call that isn't implemented stops the run and names it.

## Run it

```bash
scripts/build.sh "/path/to/Patrician III"
```

```bash
runtime/build.sh
```

```bash
out/boot "/path/to/Patrician III"
```

The window scales the game's picture to fit and keeps its shape (black bars otherwise). It can be resized and made
full screen (Ctrl-Cmd-F). Cmd-Q or the close button quits.

Environment switches:
- `P3_HEADLESS=1`: no window (tests).
- `P3_FRAMES=dir`: write every new picture the game presents to `dir/frame_NNNN.png`, at most twice a second.
- `P3_INPUT="4:click 160 388;12:quit"`: scripted input in game pixels, for tests. See `runtime/input.c`.
- `P3_WINDOW_DUMP=file.png`: once a second, write what Metal drew (read back from the drawable).
- `P3_NO_ACTIVATE=1`: don't bring the window to the front.
- `P3_RESOLUTION=WxH[xBPP]`: the game's mode. The default is the largest it supports, 1280x1024x32 (see Display).
- `BOOT_TRACE=1`: log every import and COM call with arguments and results.
- `BOOT_TIMEOUT=s`: stop after s seconds.
- `BOOT_COUNTS=file`: how often each import was called.
- `BOOT_USERDATA=dir`: use this folder instead of `~/Library/Application Support/patrician3-mac`.
- `P3_MUTE=1`: no sound.
- `P3_NO_CATCH=1`: a hardware exception stops the run instead of reaching the game's `catch(...)`.
- Heap tests: `P3_HEAP_ZERO=1` (every allocation zeroed), `P3_HEAP_NOREUSE=1` (freed blocks never reused).
- `P3_WATCH`: report writes to a guest address (see `boot.c`).

Started without a folder (as `Patrician III.app` does), `boot` uses the game files in the app's
`Contents/Resources/Game` and writes its log to `~/Library/Logs/Patrician III.log`.

The game folder is only read. Everything the game writes (settings, the registry, saves) goes to the user folder,
and reads look there first.

## Where it stands

**Playable.** A long hand-played session had no crash: menus, the town view with scrolling, buildings (the bath),
sound and music, and moving between towns, including going back to a town already visited.
- The Metal picture, read back from the drawable, is correct at Retina size.
- Not checked yet: saving and loading, long sea voyages, videos, keyboard typing.

Fixed since the first in-game runs:
- Divide-by-zero in button animations: `PathMatchSpecA` treated `\` as an escape, so wildcard texture lists
  (`images\frames_konto\konto00*.tga`) matched nothing in the archive. Those empty textures led to the divides and
  to the crash when leaving town.
- Hardware exceptions now reach C++ `catch(...)` as on Windows (`runtime/eh.c`, resumable functions from the build).
- Sound: Miles on CoreAudio (`runtime/mss32.c`).
- Crashes when a second town loaded: the game keeps pointers to freed objects of the previous town, and the heap
  handed those blocks out for objects of another size. `heap.c` now reuses blocks as Windows XP does: a freed block
  under 1 KB comes back, most recently freed first, only for the same size in 8-byte steps (the renderer also
  expects that same block back). Larger blocks use power-of-two classes.

```
boot: display 1280x1024x32
boot: AIM.dll rebased 10000000 -> 11000000 (1863 fixups)
boot: 5 modules, 17753 translated functions, 1110 imports (40 bound between modules)
boot: archiver.dll DllMain -> 1
boot: LoadLibraryA("ddraw_dll.dll") -> translated module
boot: SetDisplayMode(800x600x16)
boot: frame 0 -> out/frames/frame_0000.png          <- the main menu
```

Five modules are recompiled and linked together:

| Module | Role | Functions | Notes |
|---|---|---|---|
| `Patrician3.exe` | the game | 12,919 | |
| `ddraw_dll.dll` | SGL renderer (DirectDraw) | 2,723 | Loaded with `LoadLibrary`. Its hand-written `ASSEG` assembly rewrites its own jumps. 1,174 SSE instructions are left untranslated; the CPU we report has no SSE, so they're never reached. |
| `AIM.dll` | image loading and colour conversion | 569 | Rebased to 0x11000000. Its blitters patch their own masks and shifts. |
| `Archiver.dll` | `.cpr` archives | 1,117 | Uses memory-mapped files. |
| `Vto.dll` | `.sks` video player | 873 | Rebased to 0x12000000. Untested: no video has played yet. |
| `ijl11.dll` | Intel JPEG library | 425 | Imported by ordinal. Needs its own version resource to load. |

## Display

The game supports 800x600, 1024x768 and 1280x1024, each at 16 or 32 bits (its own mode table).
- **Default mode:** `display.c` picks the largest, 1280x1024x32. It writes that choice into the user folder's
  `P2.cfg` (`AUFLOESUNG=2`, `FARBTIEFE=1`) and `game.ini` (`[SGL] Size`, `ColDepth`) unless they were already set.
- **Hardware test:** `registry.c` marks every mode "OK", so the video test `P3Setup.exe` runs on Windows is skipped.
- **Main menu:** the game opens its menus at 800x600x16 regardless, as on Windows. The Options screen confirms the
  configured mode (1280 x 1024, 32 bit); it should apply when a game starts, which hasn't been reached yet.

1280x1024 is 5:4, so the window scales it to the screen with bars at the sides rather than stretching it.
Resolutions above the game's own table would need changes to its mode table and UI layout.

## How it works

- **`boot.c`**
  - **Loader:** maps every module at its build-time base (applying `.reloc` fixups when it was rebased) in a 4 GB guest space.
  - **Imports:** binds imports between modules; every other import goes to a thunk.
  - **Startup:** builds the TEB, PEB and stack. `DllMain` runs when a module is first needed (dependencies first),
    either at boot for the exe's static imports or through `LoadLibrary`.
  - **`dispatch()`:** routes indirect calls to translated functions, host functions or imports.
- **Win32**
  - `kernel32.c`: modules, strings, TLS, sync, time, INI files, atoms, and resources per module.
  - `heap.c`: the heap functions.
  - `files.c`: files and memory-mapped files. Views are host `mmap`s placed into guest memory.
  - `registry.c`: the registry, stored in a text file.
  - `user32.c`: a headless window manager.
  - `display.c`: display modes.
  - `extra.c`: everything else the modules import (locale, VERSION resources, GDI and dialog stubs).
- **DirectDraw**
  - `com.c` builds COM objects whose methods are host functions; a missing method stops the run with `Interface::Method`.
  - `ddraw.c` covers IDirectDraw 1/2/4, surfaces 1–4, palettes and clippers. Surfaces are guest memory.
    `Blt`/`BltFast` do copy, stretch, colour key and fills in software; `Flip` swaps the chain and presents.
  - `d3d.c` reports Direct3D requests; the menu doesn't use 3D.
- **Threads:** `threads.c`. Each guest thread is a pthread with its own CPU state, stack and TEB; one global lock lets
  one run at a time, handed over at sleeps and waits. TLS, critical sections and events live there too.
- **Miles audio:** `mss32.c` mixes samples and streams on a CoreAudio output unit; MP3 and WAV are decoded with ExtAudioFile.
- **Window and input**
  - `window.m`: a Cocoa window with a Metal layer. The game runs on the main thread and never returns to a Cocoa run
    loop, so events are taken by hand whenever the game calls `PeekMessage`, `GetMessage` or `Sleep`. Each presented
    frame is uploaded as a texture and drawn as one scaled quad.
  - `input.c`: mouse and key state (`GetCursorPos`, `GetAsyncKeyState`, ...) and the window messages for them
    (`WM_MOUSEMOVE`, button, wheel, `WM_KEYDOWN`/`WM_KEYUP`/`WM_CHAR`), from the window or from the `P3_INPUT` script.
    Mouse messages go to the window under the pointer, in its client coordinates; mouse moves are coalesced.
  - The game hides the system cursor and draws its own (a quill), so the Mac cursor is hidden over the game area.
  - Closing the window sends `WM_CLOSE`. The menu ignores it, so the port exits by itself after 2 seconds.

### Self-modifying code

`ddraw_dll` and `AIM` keep hand-written assembly in a writable section (`ASSEG`) and patch it at run time:
- AIM's colour converters write the display's shifts, masks and memory offsets into their own instructions
  (placeholders such as `[esi + 0xAAAAAAAA]`).
- The SGL rasteriser rewrites ~60 `jmp` targets to pick its MMX or plain routines.

Static translation would keep the placeholders; the first menu frame came out in garbage colours because of it.
`p3_build.py self_modifying()` finds every store with an absolute target inside code. The translator then reads
those operand fields (immediates, displacements, branch offsets) from guest memory when the instruction runs, and
the values stored into jump fields become function starts. That keeps native speed and needs no interpreter. It
only catches stores to absolute addresses; a store through a register into code would still be missed.

## Next

1. **Play more by hand** and note where it stops; the log names the missing piece.
2. **Direct3D:** requested by the renderer (`IDirect3DTexture2` from a surface) and refused; the game falls back to 2D.
3. **`SchnellTransfer.dll` (LAN):** not loaded.
4. **Videos** (`Vto.dll`, `.sks`): untested.
5. **Notarization** of the Setup app, so macOS opens it without the Privacy & Security step.
