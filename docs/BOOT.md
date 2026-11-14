# Boot runtime (2026-11-14)

`runtime/` links the translated modules into one arm64 program, `out/boot`, and runs the game's own startup
natively. It has no window and no drawing yet. Every Windows API call the game makes goes to the small Win32
subset in `runtime/`; the first call that isn't implemented stops the run and names it.

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

Environment switches:
- `BOOT_TRACE=1`: log every import call with its arguments and result.
- `BOOT_TIMEOUT=s`: stop after s seconds.
- `BOOT_COUNTS=file`: write how often each import was called.
- `BOOT_USERDATA=dir`: use this folder instead of `~/Library/Application Support/patrician3-mac`.

The game folder is only read. Everything the game writes (folders, INI changes, the registry, later saves) goes to
the user folder, and reads look there first.

## Where it stands

```
boot: 2 modules, 13087 translated functions, 585 imports (15 bound between modules)
boot: archiver.dll DllMain -> 1
boot: LoadLibraryA("ddraw_dll.dll") -> not available yet
boot: MessageBox "Patrician 3": Patrician 3: Error loading SGL settings.
boot: ExitProcess(0) after 6675 import calls
```

What runs, in order, all as translated code:
1. **`Archiver.dll` is loaded and initialised.** It is recompiled like the exe (827 Ghidra functions → 1,006
   translated). Its 15 exports are bound straight into `Patrician3.exe`'s import table, and its `DllMain` runs first.
2. **C runtime startup:** heap, TLS, locale and code pages, environment, command line.
3. **The C++ static constructors:** 551 entries in the `_initterm` tables, including one that opens a `.cpr`
   archive through Archiver.
4. **MFC application startup.**
5. **The hardware check.** The game reads `HKLM\SOFTWARE\Ascaron\Patrizier 2`. `runtime/registry.c` seeds
   `Adapter` and marks the usual video modes `"OK"` (the game's table is UNTESTED / OK / INAVAILABLE). Without this,
   the game asks for `P2Setup.exe` and quits.
6. **The `[SGL]` settings from `game.ini`:** resolution 800×600, among others.
7. **`LoadLibraryA("ddraw_dll.dll")`**, the renderer. It isn't available, so the game reports
   "Error loading SGL settings", posts `WM_QUIT`, runs every destructor and exits with code 0.

The orderly shutdown matters as evidence: thousands of translated functions (constructors, MFC, the archive layer,
then the destructors) ran without a translation fault.

## How it works

- **`boot.c`**
  - **Loader:** maps every module at its own base address in a 4 GB guest space.
  - **Import binding:** a slot naming another loaded module's export points at that export; every other slot
    points at a thunk.
  - **Startup:** builds the TEB (`fs:`), the PEB and a 4 MB stack, runs each DLL's `DllMain`, then the exe's entry point.
  - **`dispatch()`:** routes indirect calls to translated functions (binary search over every module, with a cache),
    to host functions, or to imports.
- **Win32 subset**
  - `kernel32.c`: version, modules (`GetModuleHandle`, `LoadLibrary` and `GetProcAddress` know about translated modules),
    strings and code pages, TLS, synchronisation (single-threaded for now), time, INI files, atoms, PE resources.
  - `heap.c`: Heap*, Global*, Local* and VirtualAlloc, using size classes with free lists.
  - `files.c`: Windows paths resolved case-insensitively against the user folder, then the game folder.
  - `registry.c`: `user_dir/registry.txt`.
  - `user32.c`: a headless window manager. Classes and windows, the CBT hook MFC uses to attach `CWnd` objects,
    `SendMessage`/`PostMessage`, `GetMessage`/`PeekMessage`, timers and properties, `LoadString` from the exe's
    resources, `wsprintf`, and GDI calls that succeed without drawing anything.
- **Build**
  - `recomp/p3_build.py --module NAME` builds any module into `out/build/NAME/`, and also emits its exports.
  - `scripts/build.sh` builds the exe plus every DLL that has a manifest (`manifest/<module>_functions.tsv`).
  - `runtime/build.sh` links every module it finds.

## Next

1. **The renderer, `ddraw_dll.dll`: 198 `sgl_*` exports.** Two routes:
   - Recompile it like Archiver. It is then a DirectDraw/Direct3D 7 client, so the runtime needs DirectDraw and D3D7
     on Metal.
   - Implement the `sgl_*` API directly on Metal as host functions that `GetProcAddress` returns. This means less
     emulation, but the 198 functions need to be understood first; their names and the game's call sites are the guide.

   Either way, the first visible milestone is a window that shows the game's loading screen.
2. **Real windows and input:** a Cocoa window behind the main game window, and mouse and keyboard turned into the
   game's messages.
3. **The other game DLLs:**
   - `mss32.dll` (Miles audio): reimplement the 43 `AIL_*` calls on CoreAudio.
   - `VTO.dll` (video), `AIM.dll` (images) and `SchnellTransfer.dll` (LAN): recompile or reimplement. Recompiling needs
     relocation, since all three share base 0x10000000 with Archiver.
4. **Threads.** `CreateThread` is only recorded so far; the game starts a few.
5. **Structured exception handling and C++ `throw`.** `RaiseException` and `RtlUnwind` stop the run if they are reached.
