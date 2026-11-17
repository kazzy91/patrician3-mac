// heap.c - guest heap for HeapAlloc / GlobalAlloc / LocalAlloc / VirtualAlloc.
//
// Bump allocation in [HEAP_BASE, HEAP_END) with power-of-two size classes and per-class free lists. Each block has a
// 16-byte header just below it: requested size, class, magic, and a Windows-like heap entry word (see heap_alloc). Simple, but enough to boot; the NFS port's slab heap
// can replace it if fragmentation shows up.
#include <stdlib.h>

#include "host.h"

#define MAGIC 0x50334850u          // "P3HP"
static uint32_t next = HEAP_BASE;
// Reuse as Windows XP's heap does for the game's objects: a freed small block (under 1 KB) comes back, most recently
// freed first, only for an allocation of the same size in 8-byte steps; larger blocks use power-of-two classes (so
// growing buffers often stay in place on realloc). Patrician III depends on both sides of this:
//   - it keeps stale pointers to freed walkers of the previous town; reused for an object of another size, they read
//     garbage (out-of-range object ids, crashes when a second town loaded);
//   - its renderer frees and reallocates same-size objects and expects the same block back.
#define SMALL 128                  // classes 1..128: 8..1024 bytes
static uint32_t free_head[SMALL + 33];
static uint32_t class_size(unsigned k) { return k <= SMALL ? 8u * k : 1u << (k - SMALL); }
static unsigned class_of(uint32_t n) {
    if (n <= 8u * SMALL) return n ? (n + 7) / 8 : 1;
    unsigned k = 11;
    while ((1u << k) < n) ++k;
    return SMALL + k;
}

uint32_t heap_alloc(uint32_t n, int zero) {
    unsigned k = class_of(n ? n : 1);
    if (k > SMALL + 31) die("heap_alloc(%u): too large", n);
    uint32_t a = free_head[k];
    if (a) {
        free_head[k] = M32(a);
    } else {
        uint32_t need = 16 + class_size(k);
        if ((uint64_t)next + need > HEAP_END) die("guest heap full (%u MB used)", (next - HEAP_BASE) >> 20);
        a = next + 16;
        next += need;
    }
    M32(a - 16) = n;
    M32(a - 12) = k;
    M32(a - 8) = MAGIC;
    // The dword just before a block, as Windows XP's heap leaves it: SmallTagIndex 0, Flags = busy, UnusedBytes,
    // SegmentIndex 0. The game reads it: an empty list (scripts\buttons.ini is missing from the game data) is indexed
    // at -1, and Windows hands back this header word, never 0. A zero here made the game divide by zero.
    M32(a - 4) = 0x0100u | (uint32_t)(((n ? (8 - n % 8) % 8 : 0) + 8) & 0xFF) << 16;
    static int always = -1;
    if (always < 0) always = getenv("P3_HEAP_ZERO") != NULL;   // test: does the game rely on fresh memory being zero?
    if (zero || always) memset(G + a, 0, n);
    return a;
}

static int valid(uint32_t a) {
    return a >= HEAP_BASE + 16 && a < next && M32(a - 8) == MAGIC;
}

void heap_free(uint32_t a) {
    if (!a) return;
    if (!valid(a)) { trace("  heap_free(%08X): not a heap block, ignored\n", a); return; }
    static int keep = -1;
    if (keep < 0) keep = getenv("P3_HEAP_NOREUSE") != NULL;   // test: freed blocks are never reused (stale pointers keep their data)
    if (keep) return;
    unsigned k = M32(a - 12);
    M32(a - 8) = 0;                                        // no longer a valid block (double frees are ignored)
    M32(a) = free_head[k];
    free_head[k] = a;
}

uint32_t heap_size(uint32_t a) { return valid(a) ? M32(a - 16) : 0xFFFFFFFFu; }

uint32_t heap_realloc(uint32_t a, uint32_t n, int zero) {
    if (!a) return heap_alloc(n, zero);
    if (!valid(a)) return 0;
    uint32_t old = M32(a - 16);
    if (n <= class_size(M32(a - 12))) {      // fits in place
        if (zero && n > old) memset(G + a + old, 0, n - old);
        M32(a - 16) = n;
        return a;
    }
    uint32_t b = heap_alloc(n, zero);
    memcpy(G + b, G + a, old < n ? old : n);
    heap_free(a);
    return b;
}

uint32_t heap_pages(uint32_t n) {
    next = (next + 0xFFFF) & ~0xFFFFu;
    uint32_t a = next;
    uint32_t need = (n + 0xFFFF) & ~0xFFFFu;
    if ((uint64_t)a + need > HEAP_END) die("guest heap full in VirtualAlloc(%u)", n);
    next += need;
    memset(G + a, 0, n);
    return a;
}

uint32_t gstr(const char *s) {
    uint32_t a = heap_alloc((uint32_t)strlen(s) + 1, 0);
    strcpy((char *)G + a, s);
    return a;
}

#define F(name) if (!strcmp(fn, name))
int win32_heap(Cpu *c, const char *fn) {
    F("GetProcessHeap") RET(0x00010000, 0);
    F("HeapCreate") RET(0x00010000, 3);
    F("HeapDestroy") RET(1, 1);
    F("HeapAlloc") RET(heap_alloc(arg(c, 2), arg(c, 1) & 8), 3);              // HEAP_ZERO_MEMORY = 8
    F("HeapFree") { heap_free(arg(c, 2)); RET(1, 3); }
    F("HeapReAlloc") RET(heap_realloc(arg(c, 2), arg(c, 3), arg(c, 1) & 8), 4);
    F("HeapSize") RET(heap_size(arg(c, 2)), 3);
    F("HeapValidate") RET(1, 3);
    // Global/Local memory: handles are the block addresses (GMEM_MOVEABLE is treated as fixed).
    F("GlobalAlloc") RET(heap_alloc(arg(c, 1) ? arg(c, 1) : 1, arg(c, 0) & 0x40), 2);   // GMEM_ZEROINIT = 0x40
    F("LocalAlloc") RET(heap_alloc(arg(c, 1) ? arg(c, 1) : 1, arg(c, 0) & 0x40), 2);
    F("GlobalReAlloc") RET(heap_realloc(arg(c, 0), arg(c, 1), arg(c, 2) & 0x40), 3);
    F("LocalReAlloc") RET(heap_realloc(arg(c, 0), arg(c, 1), arg(c, 2) & 0x40), 3);
    F("GlobalFree") { heap_free(arg(c, 0)); RET(0, 1); }
    F("LocalFree") { heap_free(arg(c, 0)); RET(0, 1); }
    F("GlobalLock") RET(arg(c, 0), 1);
    F("LocalLock") RET(arg(c, 0), 1);
    F("GlobalUnlock") RET(1, 1);
    F("LocalUnlock") RET(1, 1);
    F("GlobalHandle") RET(arg(c, 0), 1);
    F("GlobalSize") RET(heap_size(arg(c, 0)), 1);
    F("LocalSize") RET(heap_size(arg(c, 0)), 1);
    F("GlobalFlags") RET(0, 1);
    F("VirtualAlloc") {
        uint32_t want = arg(c, 0), n = arg(c, 1);
        if (want) RET(want, 4);                // commit of an earlier reservation: already backed
        RET(heap_pages(n), 4);
    }
    F("VirtualFree") RET(1, 3);
    F("GlobalMemoryStatus") {
        uint32_t p = arg(c, 0);
        M32(p) = 32; M32(p + 4) = 10;
        M32(p + 8) = 0x7FFFFFFF; M32(p + 12) = 0x40000000;    // 2 GB physical, 1 GB free
        M32(p + 16) = 0x7FFFFFFF; M32(p + 20) = 0x40000000;
        M32(p + 24) = 0x7FFE0000; M32(p + 28) = 0x40000000;
        RET(0, 1);
    }
    return 0;
}
