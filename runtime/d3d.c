// d3d.c - Direct3D for ddraw_dll.dll. First step: report which interfaces the renderer asks for.
#include "com.h"

int d3d_query_dd(Cpu *c, uint32_t iid, uint32_t out) {
    (void)c; (void)out;
    char s[40];
    guid_str(iid, s, sizeof s);
    fprintf(stderr, "boot: Direct3D interface requested from IDirectDraw: %s\n", s);
    return 0;
}
int d3d_query_surface(Cpu *c, unsigned surf, uint32_t iid, uint32_t out) {
    (void)c; (void)out;
    char s[40];
    guid_str(iid, s, sizeof s);
    fprintf(stderr, "boot: Direct3D interface requested from surface #%u: %s\n", surf, s);
    return 0;
}
