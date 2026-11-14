// com.c - guest COM objects (see com.h).
#include <stdlib.h>

#include "com.h"

static int missing(Cpu *c) { (void)c; return 0; }   // host_fn's name says which method; dispatch() stops the run

uint32_t com_vtable(const char *iface, const char *const *names, unsigned n, const ComMethod *impl, unsigned n_impl) {
    uint32_t vt = heap_alloc(4 * n, 1);
    for (unsigned i = 0; i < n; ++i) {
        HostFn fn = missing;
        for (unsigned k = 0; k < n_impl; ++k) if (!strcmp(impl[k].name, names[i])) fn = impl[k].fn;
        char *full = malloc(strlen(iface) + strlen(names[i]) + 3);
        sprintf(full, "%s::%s", iface, names[i]);
        // host_fn deduplicates on (fn, name), so every slot gets its own address and a readable name
        M32(vt + 4 * i) = host_fn(fn, full);
    }
    return vt;                         // implementations for methods of later versions are simply not used here
}

uint32_t com_new(uint32_t vtable, uint32_t host_id) {
    uint32_t o = heap_alloc(16, 1);
    M32(o) = vtable;
    M32(o + 4) = 1;
    M32(o + 8) = host_id;
    return o;
}

int com_addref(Cpu *c) { uint32_t o = com_this(c); RET(++M32(o + 4), 1); }
int com_release(Cpu *c) { uint32_t o = com_this(c); if (M32(o + 4)) --M32(o + 4); RET(M32(o + 4), 1); }

void guid_str(uint32_t g, char *out, size_t n) {
    snprintf(out, n, "%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X", M32(g), M16(g + 4), M16(g + 6),
             M8(g + 8), M8(g + 9), M8(g + 10), M8(g + 11), M8(g + 12), M8(g + 13), M8(g + 14), M8(g + 15));
}
int guid_eq(uint32_t g, const char *text) {
    char s[40];
    if (!g) return 0;
    guid_str(g, s, sizeof s);
    return !strcasecmp(s, text);
}
