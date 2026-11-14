// boot.c - native boot of the recompiled Patrician III (out/build/<module>/<module>.a).
//
// Loads every translated module (Patrician3.exe and the game DLLs that are recompiled, e.g. Archiver.dll) into guest
// memory at its own base, binds imports between them, points every other IAT slot at a synthetic thunk, builds a TEB
// (fs:) and a stack, runs each DLL's entry point, then the exe's. Imports outside the loaded modules are served by
// the Win32 subset in this folder; the first unimplemented one stops the run with its name, which is the to-do list
// for the host layer. Structure follows nfs-most-wanted-mac runtime/boot.c (MIT): see NOTICE.
//
//   ./boot "/path/to/Patrician III"     (BOOT_TRACE=1: every import call; BOOT_TIMEOUT=s; BOOT_COUNTS=file)
#include <dirent.h>
#include <errno.h>
#include <mach/mach_time.h>
#include <signal.h>
#include <stdarg.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include "host.h"
#include "module.h"

uint8_t *G;
int boot_trace;
const char *game_dir, *user_dir;
__thread uint32_t last_error;
unsigned import_calls;
static Cpu *g_cpu;
static const char *last_import = "(none)";

void die(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "boot: ");
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n");
    if (g_cpu)
        fprintf(stderr, "boot: after %u import calls, last %s; eax %08X ecx %08X edx %08X esp %08X [esp] %08X\n",
                import_calls, last_import, g_cpu->eax, g_cpu->ecx, g_cpu->edx, g_cpu->esp, M32(g_cpu->esp));
    exit(1);
}

void trace(const char *fmt, ...) {
    if (!boot_trace) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

// A host fault inside translated code: a guest access outside mapped memory can't happen (all 4 GB are reserved), so
// this is a write to a read-only host page, a stack overflow, or a bug in the host layer. Reports the guest address
// when the fault is inside guest memory, and the host call stack (translated functions are named f_<guest address>).
#include <execinfo.h>
static void on_fault(int sig, siginfo_t *info, void *ctx) {
    (void)ctx;
    uintptr_t a = (uintptr_t)info->si_addr, g = (uintptr_t)G;
    fprintf(stderr, "boot: host fault (signal %d) at %p", sig, info->si_addr);
    if (G && a >= g && a < g + (1ull << 32)) fprintf(stderr, " = guest address %08X", (uint32_t)(a - g));
    fprintf(stderr, "; %u import calls, last %s\n", import_calls, last_import);
    void *frames[48];
    int n = backtrace(frames, 48);
    backtrace_symbols_fd(frames, n, 2);
    _exit(139);
}

static void on_timeout(int sig) {
    (void)sig;
    die("BOOT_TIMEOUT reached");
}

// ---------------- hooks the translated code calls ----------------
void untranslated(Cpu *c, uint32_t addr) { (void)c; die("reached untranslated code at %08X", addr); }
void bad_jump(Cpu *c, uint32_t at) { (void)c; die("jump table index out of range at %08X", at); }
void guest_breakpoint(Cpu *c, uint32_t at) { (void)c; trace("  int3 at %08X (continuing)\n", at); }
void guest_divide_error(Cpu *c, uint32_t at) { (void)c; fprintf(stderr, "boot: #DE (divide error) at %08X, continuing\n", at); }
void host_cpuid(Cpu *c) {
    // A generic Pentium III class CPU: FPU, TSC, CX8, CMOV, MMX. No SSE (the translator handles MMX only).
    switch (c->eax) {
    case 0: c->eax = 1; c->ebx = 0x756E6547; c->edx = 0x49656E69; c->ecx = 0x6C65746E; break;   // "GenuineIntel"
    case 1: c->eax = 0x00000673; c->ebx = 0; c->ecx = 0; c->edx = getenv("P3_NO_MMX") ? 0x0000FBFF : 0x0080FBFF; break;
    default: c->eax = c->ebx = c->ecx = c->edx = 0;
    }
}
uint64_t host_ticks(void) {
    static mach_timebase_info_data_t tb;
    if (!tb.denom) mach_timebase_info(&tb);
    uint64_t t = mach_absolute_time();
    return tb.numer == tb.denom ? t : t / tb.denom * tb.numer + t % tb.denom * tb.numer / tb.denom;
}
uint64_t host_filetime(void) {
    static uint64_t ft0, t0;
    if (!ft0) {
        struct timeval tv;
        gettimeofday(&tv, NULL);
        t0 = host_ticks();
        ft0 = (uint64_t)tv.tv_sec * 10000000ull + (uint64_t)tv.tv_usec * 10ull + 116444736000000000ull;
    }
    return ft0 + (host_ticks() - t0) / 100;
}

// ---------------- modules ----------------
const ModuleTables *module_by_name(const char *name) {
    char want[260];
    const char *s = strrchr(name, '\\');
    snprintf(want, sizeof want, "%s", s ? s + 1 : name);
    for (unsigned i = 0; i < module_count; ++i) {
        const ModuleTables *m = all_modules[i];
        size_t n = strlen(want);
        if (!strcasecmp(want, m->file) || !strcasecmp(want, m->name)) return m;
        if (!strchr(want, '.') && !strncasecmp(m->file, want, n) && m->file[n] == '.') return m;   // "Archiver" = "Archiver.dll"
    }
    return NULL;
}
const ModuleTables *module_by_base(uint32_t base) {
    for (unsigned i = 0; i < module_count; ++i) if (all_modules[i]->base == base) return all_modules[i];
    return NULL;
}
uint32_t module_export(const ModuleTables *m, const char *name) {
    for (unsigned i = 0; i < m->export_count; ++i) if (!strcmp(m->exports[i].name, name)) return m->exports[i].addr;
    return 0;
}

// Every IAT slot of every module, sorted by slot address; thunk i is THUNK_BASE + 4*i.
typedef struct { uint32_t slot; const char *name; uint64_t calls; } Import;
static Import *imports;
static unsigned n_imports;
static int import_cmp(const void *a, const void *b) {
    uint32_t x = ((const Import *)a)->slot, y = ((const Import *)b)->slot;
    return x < y ? -1 : x > y;
}

// ---------------- host functions and guest calls ----------------
static HostFn host_fns[4096];
static const char *host_fn_names[4096];
static unsigned host_fn_count;
uint32_t host_fn(HostFn fn, const char *name) {
    for (unsigned i = 0; i < host_fn_count; ++i)
        if (host_fns[i] == fn && !strcmp(host_fn_names[i], name)) return HOSTFN_BASE + 4 * i;
    if (host_fn_count == 4096) die("too many host functions");
    host_fns[host_fn_count] = fn;
    host_fn_names[host_fn_count] = name;
    return HOSTFN_BASE + 4 * host_fn_count++;
}
uint32_t call_guest(Cpu *c, uint32_t fn, int n, const uint32_t *args) {
    uint32_t sp = c->esp;
    for (int i = n - 1; i >= 0; --i) { c->esp -= 4; M32(c->esp) = args[i]; }
    c->esp -= 4;
    M32(c->esp) = 0xFEEDF00D;              // return address (unused: translated code returns to C)
    dispatch(c, fn);
    c->esp = sp;                           // stdcall callees already popped; cdecl ones didn't
    return c->eax;
}

static void import_call(Cpu *c, unsigned index);

void dispatch(Cpu *c, uint32_t t) {
    if (t >= THUNK_BASE && t < THUNK_BASE + 4 * n_imports) { import_call(c, (t - THUNK_BASE) / 4); return; }
    if (t >= HOSTFN_BASE && t < HOSTFN_BASE + 4 * host_fn_count) {
        unsigned i = (t - HOSTFN_BASE) / 4;
        trace("  [host] %s  [from %08X]\n", host_fn_names[i], M32(c->esp));
        if (!host_fns[i](c)) die("not implemented yet: %s (called from %08X)", host_fn_names[i], M32(c->esp));
        return;
    }
    static struct { uint32_t addr; void (*fn)(Cpu *); } cache[1 << 14];
    uint32_t h = (t >> 4 ^ t >> 17) & ((1 << 14) - 1);
    if (cache[h].addr == t && cache[h].fn) { cache[h].fn(c); return; }
    for (unsigned k = 0; k < module_count; ++k) {
        const ModuleTables *m = all_modules[k];
        if (!m->func_count || t < m->func_addr[0] || t > m->func_addr[m->func_count - 1]) continue;
        int lo = 0, hi = (int)m->func_count - 1;
        while (lo <= hi) {
            int mid = (lo + hi) / 2;
            if (m->func_addr[mid] == t) { cache[h].addr = t; cache[h].fn = m->func_ptr[mid]; m->func_ptr[mid](c); return; }
            if (m->func_addr[mid] < t) lo = mid + 1; else hi = mid - 1;
        }
    }
    die("indirect call to %08X, which is not a known function (return address %08X)", t, M32(c->esp));
}

static void write_call_counts(void) {
    if (!getenv("BOOT_COUNTS")) return;
    FILE *f = fopen(getenv("BOOT_COUNTS"), "w");
    if (!f) return;
    for (unsigned i = 0; i < n_imports; ++i)
        if (imports[i].calls) fprintf(f, "%10llu  %s\n", (unsigned long long)imports[i].calls, imports[i].name);
    fclose(f);
}

static void import_call(Cpu *c, unsigned index) {
    const char *name = imports[index].name, *bang = strchr(name, '!');
    const char *fn = bang + 1;
    char dll[64];
    snprintf(dll, sizeof dll, "%.*s", (int)(bang - name), name);
    ++import_calls;
    ++imports[index].calls;
    last_import = name;
    uint32_t ra = M32(c->esp), a0 = arg(c, 0), a1 = arg(c, 1), a2 = arg(c, 2), a3 = arg(c, 3);
    gil_maybe_yield();
    int ok = !strcasecmp(dll, "KERNEL32.dll") ? win32_threads(c, fn) || win32_heap(c, fn) || win32_files(c, fn) || win32_kernel32(c, fn)
           : !strcasecmp(dll, "USER32.dll") ? win32_display(c, fn) || win32_input(c, fn) || win32_user32(c, fn)
           : !strcasecmp(dll, "GDI32.dll") ? win32_gdi32(c, fn)
           : !strcasecmp(dll, "DDRAW.dll") ? win32_ddraw(c, fn)
           : !strcasecmp(dll, "mss32.dll") ? win32_mss32(c, fn)
           : win32_misc(c, dll, fn);
    if (!ok) ok = win32_extra(c, dll, fn);
    if (!ok) die("not implemented yet: %s (call %u, return address %08X, args %08X %08X %08X %08X)",
                 name, import_calls, ra, a0, a1, a2, a3);
    trace("  [%u] %s(%X, %X, %X, %X) -> %X  @%08X\n", import_calls, name, a0, a1, a2, a3, c->eax, ra);
}

// `call [IAT slot]` in translated code lands here with the slot's address. A slot bound to another translated
// module's export holds that function's address; every other slot holds its thunk.
void host_import(Cpu *c, uint32_t slot) {
    uint32_t t = M32(slot);
    if (t < THUNK_BASE || t >= THUNK_BASE + 4 * n_imports) { dispatch(c, t); return; }
    import_call(c, (t - THUNK_BASE) / 4);
}

// ---------------- loader ----------------
static uint8_t *read_module(const char *file, long *size) {
    // case-insensitive lookup in the game folder
    char path[4096] = "";
    DIR *d = opendir(game_dir);
    struct dirent *e;
    while (d && (e = readdir(d)))
        if (!strcasecmp(e->d_name, file)) { snprintf(path, sizeof path, "%s/%s", game_dir, e->d_name); break; }
    if (d) closedir(d);
    if (!path[0]) die("%s not found in %s", file, game_dir);
    FILE *f = fopen(path, "rb");
    if (!f) die("cannot open %s: %s", path, strerror(errno));
    fseek(f, 0, SEEK_END);
    *size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *p = malloc(*size);
    if (fread(p, 1, *size, f) != (size_t)*size) die("cannot read %s", path);
    fclose(f);
    return p;
}

static void load_module(const ModuleTables *m) {
    long size;
    uint8_t *pe = read_module(m->file, &size);
    uint32_t off = *(uint32_t *)(pe + 0x3C);
    uint16_t nsec = *(uint16_t *)(pe + off + 6), optsz = *(uint16_t *)(pe + off + 20);
    uint32_t file_base = *(uint32_t *)(pe + off + 24 + 28), image_size = *(uint32_t *)(pe + off + 24 + 56);
    uint32_t base = m->base;                       // where it was translated (p3_build.py --base rebases)
    for (unsigned i = 0; i < module_count && all_modules[i] != m; ++i) {
        const ModuleTables *o = all_modules[i];
        uint32_t ob = o->base, oe = ob + M32(ob + M32(ob + 0x3C) + 24 + 56);
        if (base < oe && ob < base + image_size)
            die("%s (%08X..%08X) overlaps %s: give it a free base in manifest/bases.tsv", m->file, base, base + image_size, o->file);
    }
    memcpy(G + base, pe, 0x1000);                                       // headers
    for (int i = 0; i < nsec; ++i) {
        uint8_t *s = pe + off + 24 + optsz + i * 40;
        uint32_t va = *(uint32_t *)(s + 12), rs = *(uint32_t *)(s + 16), raw = *(uint32_t *)(s + 20);
        memcpy(G + base + va, pe + raw, rs);
    }
    if (base != file_base) {
        // The same HIGHLOW fixups p3_build.py applied before translation (.reloc, data directory 5).
        uint32_t rva = *(uint32_t *)(pe + off + 24 + 96 + 5 * 8), size = *(uint32_t *)(pe + off + 24 + 96 + 5 * 8 + 4);
        uint32_t delta = base - file_base, p = base + rva, end = p + size, n = 0;
        if (!rva) die("%s: rebased but has no relocation table", m->file);
        while (p < end) {
            uint32_t page = M32(p), block = M32(p + 4);
            if (block < 8) break;
            for (uint32_t k = 0; k < (block - 8) / 2; ++k) {
                uint16_t e = M16(p + 8 + 2 * k);
                if (e >> 12 == 3) { M32(base + page + (e & 0xFFF)) += delta; ++n; }
                else if (e >> 12) die("%s: relocation type %u", m->file, e >> 12);
            }
            p += block;
        }
        M32(base + off + 24 + 28) = base;
        printf("boot: %s rebased %08X -> %08X (%u fixups)\n", m->file, file_base, base, n);
    }
    free(pe);
}

// DllMain(hinst, DLL_PROCESS_ATTACH, 0) once per module, after the modules it imports from.
static unsigned char inited[64];
int module_init(Cpu *c, const ModuleTables *m) {
    unsigned idx = 0;
    while (idx < module_count && all_modules[idx] != m) ++idx;
    if (idx == 0 || inited[idx]) return 1;
    inited[idx] = 1;
    for (unsigned j = 0; j < m->import_count; ++j) {
        char dll[64];
        const char *bang = strchr(m->imports[j].name, '!');
        snprintf(dll, sizeof dll, "%.*s", (int)(bang - m->imports[j].name), m->imports[j].name);
        const ModuleTables *d = module_by_name(dll);
        if (d && d != m && !module_init(c, d)) return 0;
    }
    uint32_t a[3] = {m->base, 1, 0};
    uint32_t r = call_guest(c, m->entry, 3, a);
    printf("boot: %s DllMain -> %u\n", m->file, r);
    fflush(stdout);
    return r != 0;
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s \"/path/to/Patrician III\"\n", argv[0]); return 2; }
    game_dir = argv[1];
    boot_trace = getenv("BOOT_TRACE") != NULL;
    static char ud[1024];
    snprintf(ud, sizeof ud, "%s/Library/Application Support/patrician3-mac", getenv("HOME"));
    mkdir(ud, 0755);
    user_dir = getenv("BOOT_USERDATA") ? getenv("BOOT_USERDATA") : ud;
    if (getenv("BOOT_TIMEOUT")) { signal(SIGALRM, on_timeout); alarm((unsigned)atoi(getenv("BOOT_TIMEOUT"))); }
    atexit(write_call_counts);
    static char alt_stack[1 << 16];        // its own stack, so a host stack overflow is reported too
    stack_t ss = {.ss_sp = alt_stack, .ss_size = sizeof alt_stack};
    sigaltstack(&ss, NULL);
    struct sigaction sa = {.sa_sigaction = on_fault, .sa_flags = SA_SIGINFO | SA_ONSTACK};
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);

    display_init();
    input_init();
    window_init();
    G = mmap(0, 1ull << 32, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
    if (G == MAP_FAILED) die("cannot reserve 4 GB of guest memory");
    if (all_modules[0]->base != IMAGE_BASE) die("the first module must be the exe at %08X", IMAGE_BASE);
    for (unsigned i = 0; i < module_count; ++i) load_module(all_modules[i]);

    // Imports: bind to loaded modules' exports, else to a thunk.
    for (unsigned i = 0; i < module_count; ++i) n_imports += all_modules[i]->import_count;
    imports = calloc(n_imports, sizeof *imports);
    unsigned k = 0, bound = 0;
    for (unsigned i = 0; i < module_count; ++i)
        for (unsigned j = 0; j < all_modules[i]->import_count; ++j, ++k) {
            imports[k].slot = all_modules[i]->imports[j].slot;
            imports[k].name = all_modules[i]->imports[j].name;
        }
    qsort(imports, n_imports, sizeof *imports, import_cmp);
    for (unsigned i = 0; i < n_imports; ++i) {
        char dll[64];
        const char *bang = strchr(imports[i].name, '!');
        snprintf(dll, sizeof dll, "%.*s", (int)(bang - imports[i].name), imports[i].name);
        const ModuleTables *m = module_by_name(dll);
        uint32_t a = m ? module_export(m, bang + 1) : 0;
        if (m && !a) die("%s: %s does not export %s", imports[i].name, m->file, bang + 1);
        M32(imports[i].slot) = a ? a : THUNK_BASE + 4 * i;
        bound += a != 0;
    }

    Cpu cpu = {0}, *c = &cpu;
    g_cpu = c;
    threads_init();
    c->fs_base = TEB;
    M32(TEB + 0x00) = 0xFFFFFFFF;          // SEH chain end
    M32(TEB + 0x04) = STACK_TOP;           // stack base
    M32(TEB + 0x08) = STACK_TOP - STACK_SIZE;
    M32(TEB + 0x18) = TEB;                 // self
    M32(TEB + 0x20) = 1;                   // process id
    M32(TEB + 0x24) = 1;                   // thread id
    M32(TEB + 0x30) = PEB;
    M32(PEB + 0x08) = IMAGE_BASE;          // image base
    c->esp = STACK_TOP - 0x100;
    c->cw = 0x027F;
    unsigned funcs = 0;
    for (unsigned i = 0; i < module_count; ++i) funcs += all_modules[i]->func_count;
    printf("boot: %u modules, %u translated functions, %u imports (%u bound between modules)\n", module_count, funcs, n_imports, bound);
    fflush(stdout);
    // The exe's static imports are initialised now; other modules when LoadLibrary first asks for them.
    for (unsigned j = 0; j < all_modules[0]->import_count; ++j) {
        char dll[64];
        const char *name = all_modules[0]->imports[j].name, *bang = strchr(name, '!');
        snprintf(dll, sizeof dll, "%.*s", (int)(bang - name), name);
        const ModuleTables *d = module_by_name(dll);
        if (d && !module_init(c, d)) die("%s: DllMain failed", d->file);
    }
    c->esp -= 4;
    M32(c->esp) = 0xDEADBEEF;              // entry's return address
    dispatch(c, all_modules[0]->entry);
    printf("boot: entry returned eax=%08X after %u import calls\n", c->eax, import_calls);
    return 0;
}
