// host.h - shared helpers for the host side of the runtime (boot.c, kernel32.c, user32.c, ...).
// Conventions follow nfs-most-wanted-mac runtime/host.h (MIT): see NOTICE.
#pragma once
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "runtime.h"

// Guest address map (the image itself is at its own base, 0x400000..0x6F9000).
#define HEAP_BASE   0x30000000u   // Win32 heaps, VirtualAlloc, GlobalAlloc/LocalAlloc
#define HEAP_END    0x60000000u
#define STACK_TOP   0x7FF00000u
#define STACK_SIZE  0x00400000u   // 4 MB main-thread stack
#define TEB         0x7FFDE000u
#define PEB         0x7FFDF000u
#define THUNK_BASE  0xF0000000u   // IAT slot i holds THUNK_BASE + 4*i
#define HOSTFN_BASE 0xF1000000u   // host functions callable from guest code (callbacks, GetProcAddress results)
#define IMAGE_BASE  0x00400000u

// Stack arguments of an import or host function: [esp] is the return address.
static inline uint32_t arg(Cpu *c, int i) { return M32(c->esp + 4 + 4 * i); }
static inline const char *gptr(uint32_t a) { return a ? (const char *)G + a : NULL; }
static inline uint16_t *gwptr(uint32_t a) { return a ? (uint16_t *)(G + a) : NULL; }
// Return from a __stdcall import/host function with nargs stack arguments.
#define RET(v, nargs) do { c->eax = (uint32_t)(v); c->esp += 4 + 4 * (nargs); return 1; } while (0)
// Return from a __cdecl function: the caller pops the arguments.
#define RET_CDECL(v) do { c->eax = (uint32_t)(v); c->esp += 4; return 1; } while (0)

__attribute__((noreturn, format(printf, 1, 2))) void die(const char *fmt, ...);
void trace(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
extern int boot_trace;
extern const char *game_dir;     // the user's Patrician III folder (read-only: never written)
extern const char *user_dir;     // ~/Library/Application Support/patrician3-mac: saves, settings, written files
extern __thread uint32_t last_error;
extern unsigned import_calls;

void dispatch(Cpu *c, uint32_t target);
uint64_t host_ticks(void);       // monotonic nanoseconds
uint64_t host_filetime(void);    // wall clock as FILETIME (100 ns since 1601)
uint32_t call_guest(Cpu *c, uint32_t fn, int n, const uint32_t *args);

typedef int (*HostFn)(Cpu *c);
uint32_t host_fn(HostFn fn, const char *name);

// heap.c
uint32_t heap_alloc(uint32_t n, int zero);
void heap_free(uint32_t a);
uint32_t heap_size(uint32_t a);
uint32_t heap_realloc(uint32_t a, uint32_t n, int zero);
uint32_t heap_pages(uint32_t n);   // page-aligned, zeroed (VirtualAlloc)
uint32_t gstr(const char *s);

// files.c: Windows paths -> host paths. Reads look in user_dir first, then game_dir; writes go to user_dir.
int path_resolve(const char *win, char *out, size_t n, int for_write);
void path_guest_dir(char *out, size_t n);   // the folder the game believes it runs from, "C:\Patrician3"

// Modules: each returns 1 if it handled the import `fn` (name without the DLL prefix).
int win32_kernel32(Cpu *c, const char *fn);
int win32_files(Cpu *c, const char *fn);
int win32_heap(Cpu *c, const char *fn);
int win32_user32(Cpu *c, const char *fn);
int win32_gdi32(Cpu *c, const char *fn);
int win32_misc(Cpu *c, const char *dll, const char *fn);   // SHELL32, WINMM, ... (misc.c)
int win32_advapi(Cpu *c, const char *fn);                   // registry.c
int win32_display(Cpu *c, const char *fn);                  // display.c: EnumDisplaySettings, ChangeDisplaySettings
int win32_ddraw(Cpu *c, const char *fn);                    // ddraw.c
int win32_mss32(Cpu *c, const char *fn);                    // mss32.c: Miles, silent
int win32_extra(Cpu *c, const char *dll, const char *fn);   // extra.c: the rest of the imported surface
int win32_threads(Cpu *c, const char *fn);                  // threads.c: threads, TLS, critical sections, events, waits
void threads_init(void);                                    // the calling (main) thread takes the GIL
void gil_acquire(void);
void gil_release(void);
void gil_maybe_yield(void);
int win32_input(Cpu *c, const char *fn);                    // input.c: cursor and key state
void input_init(void);                                      // input.c: P3_INPUT script
void window_init(void);                                     // window.m: the Mac window (not with P3_HEADLESS)
void display_init(void);                                    // display.c: the game resolution (P3_RESOLUTION)
extern int display_w, display_h, display_bpp;
