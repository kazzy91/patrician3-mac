// com.h - COM objects for the guest: an object is guest memory {vtable pointer, refcount, host id}; each vtable slot
// is a host function (host_fn). Slots without an implementation stop the run with "Interface::Method", which is the
// to-do list for the DirectDraw / Direct3D layer.
#pragma once
#include "host.h"

typedef struct { const char *name; HostFn fn; } ComMethod;
// Builds (once) a guest vtable for an interface from its method names (in vtable order) and the implemented methods.
uint32_t com_vtable(const char *iface, const char *const *names, unsigned n, const ComMethod *impl, unsigned n_impl);
uint32_t com_new(uint32_t vtable, uint32_t host_id);        // refcount 1
static inline uint32_t com_id(uint32_t obj) { return M32(obj + 8); }
static inline uint32_t com_this(Cpu *c) { return arg(c, 0); }
int com_addref(Cpu *c);
int com_release(Cpu *c);                                    // never frees: objects are few and long-lived
int guid_eq(uint32_t guid, const char *text);               // text: "6C14DB80-A733-11CE-A521-0020AF0BE560"
void guid_str(uint32_t guid, char *out, size_t n);
