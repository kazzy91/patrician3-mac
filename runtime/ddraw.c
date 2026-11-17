// ddraw.c - DirectDraw for ddraw_dll.dll (Ascaron's SGL engine), headless for now.
//
// One display device ("DirectDraw HAL" / "display", the adapter the registry names) at the resolution display.c
// chose. Surfaces are plain guest memory: Lock hands out a pointer, Blt/BltFast copy in software, Flip swaps the
// primary chain and presents. Presenting means, for now, writing the picture to P3_FRAMES/frame_NNNN.png when it
// changed (at most twice a second); the Metal window replaces that later. Every method not written yet stops the run
// with its name (com.c).
#include <pthread.h>
#include <stdlib.h>
#include <sys/stat.h>

#include "com.h"

#define DD_OK 0
#define E_NOINTERFACE 0x80004002u
#define DDERR_GENERIC 0x80004005u
#define DDERR_NOTFOUND 0x887600FFu
#define DDERR_INVALIDPARAMS 0x80070057u

int png_write(const char *path, const uint8_t *pixels, int w, int h, int pitch);

// ---------------- interface tables ----------------
static const char *const dd_methods[] = {
    "QueryInterface", "AddRef", "Release", "Compact", "CreateClipper", "CreatePalette", "CreateSurface",
    "DuplicateSurface", "EnumDisplayModes", "EnumSurfaces", "FlipToGDISurface", "GetCaps", "GetDisplayMode",
    "GetFourCCCodes", "GetGDISurface", "GetMonitorFrequency", "GetScanLine", "GetVerticalBlankStatus", "Initialize",
    "RestoreDisplayMode", "SetCooperativeLevel", "SetDisplayMode", "WaitForVerticalBlank",
    "GetAvailableVidMem",                                                                     // 2
    "GetSurfaceFromDC", "RestoreAllSurfaces", "TestCooperativeLevel", "GetDeviceIdentifier",   // 4
};
static const char *const surf_methods[] = {
    "QueryInterface", "AddRef", "Release", "AddAttachedSurface", "AddOverlayDirtyRect", "Blt", "BltBatch", "BltFast",
    "DeleteAttachedSurface", "EnumAttachedSurfaces", "EnumOverlayZOrders", "Flip", "GetAttachedSurface", "GetBltStatus",
    "GetCaps", "GetClipper", "GetColorKey", "GetDC", "GetFlipStatus", "GetOverlayPosition", "GetPalette",
    "GetPixelFormat", "GetSurfaceDesc", "Initialize", "IsLost", "Lock", "ReleaseDC", "Restore", "SetClipper",
    "SetColorKey", "SetOverlayPosition", "SetPalette", "Unlock", "UpdateOverlay", "UpdateOverlayDisplay",
    "UpdateOverlayZOrder",
    "GetDDInterface", "PageLock", "PageUnlock",                                               // 2
    "SetSurfaceDesc",                                                                         // 3
    "SetPrivateData", "GetPrivateData", "FreePrivateData", "GetUniquenessValue", "ChangeUniquenessValue",   // 4
};
static const char *const clipper_methods[] = {
    "QueryInterface", "AddRef", "Release", "GetClipList", "GetHWnd", "Initialize", "IsClipListChanged", "SetClipList", "SetHWnd",
};
static const char *const palette_methods[] = {
    "QueryInterface", "AddRef", "Release", "GetCaps", "GetEntries", "Initialize", "SetEntries",
};

// ---------------- state ----------------
typedef struct {
    int used;
    uint32_t w, h, pitch, bpp, mem, caps;
    uint32_t pf_flags, pf_fourcc, rmask, gmask, bmask, amask;
    uint32_t back, zbuf;              // attached surfaces (index + 1)
    uint32_t ck_src_lo, ck_src_hi, ck_flags;
    uint32_t palette, clipper, dc;
    uint32_t obj[5];                  // guest objects per interface version 1..4
    uint32_t lock_count;
} Surf;
static Surf surfs[4096];
static uint32_t dd_obj[5];
static uint32_t coop_hwnd, coop_flags;
static uint32_t mode_w, mode_h, mode_bpp;
static uint32_t primary;              // index + 1

static uint32_t dd_interface(int version);
uint32_t surf_interface(unsigned idx, int version);
int d3d_query_dd(Cpu *c, uint32_t iid, uint32_t out);         // d3d.c: IDirect3D* from IDirectDraw
int d3d_query_surface(Cpu *c, unsigned surf, uint32_t iid, uint32_t out);   // d3d.c: IDirect3DTexture* from a surface

Surf *surf_get(unsigned idx) { return idx < 4096 && surfs[idx].used ? &surfs[idx] : NULL; }
unsigned surf_index(uint32_t obj) { return obj ? com_id(obj) & 0xFFFF : 0xFFFF; }
static Surf *this_surf(Cpu *c) {
    Surf *s = surf_get(surf_index(com_this(c)));
    if (!s) die("DirectDraw surface method on an unknown object %08X", com_this(c));
    return s;
}
static int this_version(Cpu *c) { return com_id(com_this(c)) >> 16; }

// ---------------- pixel formats and descriptions ----------------
static void pf_display(Surf *s, uint32_t bpp) {
    s->bpp = bpp;
    s->pf_flags = 0x40;                                    // DDPF_RGB
    if (bpp == 32) { s->rmask = 0xFF0000; s->gmask = 0xFF00; s->bmask = 0xFF; s->amask = 0; }
    else { s->rmask = 0xF800; s->gmask = 0x07E0; s->bmask = 0x001F; s->amask = 0; }
}
static void write_pf(uint32_t p, Surf *s) {
    memset(G + p, 0, 32);
    M32(p) = 32; M32(p + 4) = s->pf_flags; M32(p + 8) = s->pf_fourcc; M32(p + 12) = s->bpp;
    M32(p + 16) = s->rmask; M32(p + 20) = s->gmask; M32(p + 24) = s->bmask; M32(p + 28) = s->amask;
}
static void read_pf(uint32_t p, Surf *s) {
    s->pf_flags = M32(p + 4); s->pf_fourcc = M32(p + 8); s->bpp = M32(p + 12);
    s->rmask = M32(p + 16); s->gmask = M32(p + 20); s->bmask = M32(p + 24); s->amask = M32(p + 28);
    if (s->pf_flags & 0x400) s->bpp = M32(p + 12);         // DDPF_ZBUFFER: depth bits in the same field
}
// DDSURFACEDESC (108 bytes) or DDSURFACEDESC2 (124), told apart by dwSize.
static void write_desc(uint32_t d, Surf *s) {
    uint32_t size = M32(d) == 124 ? 124 : 108;
    memset(G + d, 0, size);
    M32(d) = size;
    M32(d + 4) = 0x1 | 0x2 | 0x4 | 0x8 | 0x1000 | (s->back ? 0x20 : 0);   // CAPS HEIGHT WIDTH PITCH PIXELFORMAT [BACKBUFFERCOUNT]
    M32(d + 8) = s->h; M32(d + 12) = s->w; M32(d + 16) = s->pitch;
    if (s->back) M32(d + 20) = 1;
    M32(d + 36) = s->mem;
    M32(d + 64) = s->ck_src_lo; M32(d + 68) = s->ck_src_hi;
    write_pf(d + 72, s);
    M32(d + 104) = s->caps;
}

// ---------------- presenting ----------------
static uint32_t frames_written;
static uint64_t last_frame_t, last_hash;
uint32_t *present_rgb(Surf *s, uint32_t *buf) {
    // to 0x00RRGGBB, whatever the surface format
    for (uint32_t y = 0; y < s->h; ++y)
        for (uint32_t x = 0; x < s->w; ++x) {
            uint32_t p;
            if (s->bpp == 32) p = M32(s->mem + y * s->pitch + 4 * x) & 0xFFFFFF;
            else {
                uint16_t v = M16(s->mem + y * s->pitch + 2 * x);
                p = ((v >> 11) * 255 / 31) << 16 | (((v >> 5) & 63) * 255 / 63) << 8 | (v & 31) * 255 / 31;
            }
            buf[y * s->w + x] = p;
        }
    return buf;
}
void window_present(const uint32_t *xrgb, int w, int h);      // window.m
int window_active(void);
void ddraw_display_size(int *w, int *h) {
    *w = mode_w ? (int)mode_w : display_w;
    *h = mode_h ? (int)mode_h : display_h;
}
// Frames for the window: the game draws on several threads, but Cocoa and Metal belong to the main thread. Any thread
// leaves its latest picture here; the main thread shows it (ddraw_flush_present, from window_pump and from here).
static pthread_mutex_t pending_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t *pending, pending_cap;
static int pending_w, pending_h, pending_dirty;
void ddraw_flush_present(void) {
    pthread_mutex_lock(&pending_lock);
    if (pending_dirty) { window_present(pending, pending_w, pending_h); pending_dirty = 0; }
    pthread_mutex_unlock(&pending_lock);
}
static void present(Surf *s) {
    if (window_active()) {
        pthread_mutex_lock(&pending_lock);
        if (pending_cap < s->w * s->h) { free(pending); pending_cap = s->w * s->h; pending = malloc(4 * pending_cap); }
        present_rgb(s, pending);
        pending_w = (int)s->w; pending_h = (int)s->h; pending_dirty = 1;
        pthread_mutex_unlock(&pending_lock);
        if (pthread_main_np()) ddraw_flush_present();
    }
    const char *dir = getenv("P3_FRAMES");
    if (!dir) return;
    uint64_t now = host_ticks();
    if (now - last_frame_t < 500000000ull) return;
    uint64_t h = 1469598103934665603ull;
    for (uint32_t y = 0; y < s->h; y += 7) for (uint32_t x = 0; x < s->pitch; x += 61) h = (h ^ M8(s->mem + y * s->pitch + x)) * 1099511628211ull;
    if (h == last_hash) return;
    last_hash = h;
    last_frame_t = now;
    mkdir(dir, 0755);
    uint32_t *buf = malloc(4 * s->w * s->h);
    present_rgb(s, buf);
    char path[1100];
    snprintf(path, sizeof path, "%s/frame_%04u.png", dir, frames_written++);
    png_write(path, (uint8_t *)buf, s->w, s->h, 4 * s->w);
    free(buf);
    if (getenv("P3_FRAMES_RAW")) {                         // the surface bytes as they are, for format debugging
        snprintf(path, sizeof path, "%s/frame_%04u_%ux%u_%ubpp.raw", dir, frames_written - 1, s->w, s->h, s->bpp);
        FILE *f = fopen(path, "wb");
        if (f) { for (uint32_t y = 0; y < s->h; ++y) fwrite(G + s->mem + y * s->pitch, 1, s->w * s->bpp / 8, f); fclose(f); }
    }
    printf("boot: frame %u -> %s\n", frames_written - 1, path);
    fflush(stdout);
}

// ---------------- IDirectDraw ----------------
static int dd_query_interface(Cpu *c) {
    uint32_t iid = arg(c, 1), out = arg(c, 2);
    char s[40];
    guid_str(iid, s, sizeof s);
    int v = guid_eq(iid, "6C14DB80-A733-11CE-A521-0020AF0BE560") ? 1
          : guid_eq(iid, "B3A6F3E0-2B43-11CF-A2DE-00AA00B93356") ? 2
          : guid_eq(iid, "9C59509A-39BD-11D1-8C4A-00C04FD930C5") ? 4 : 0;
    if (v) { M32(out) = dd_interface(v); trace("      IDirectDraw::QueryInterface(%s) -> IDirectDraw%d\n", s, v); RET(DD_OK, 3); }
    if (d3d_query_dd(c, iid, out)) RET(DD_OK, 3);
    fprintf(stderr, "boot: IDirectDraw::QueryInterface(%s): interface not provided yet\n", s);
    M32(out) = 0;
    RET(E_NOINTERFACE, 3);
}
static int dd_set_cooperative_level(Cpu *c) {
    coop_hwnd = arg(c, 1);
    coop_flags = arg(c, 2);
    trace("      SetCooperativeLevel(hwnd %08X, flags %X)\n", coop_hwnd, coop_flags);
    RET(DD_OK, 3);
}
static const struct { int w, h; } dd_modes[] = {{640, 480}, {800, 600}, {1024, 768}, {1280, 1024}};
static int dd_enum_display_modes(Cpu *c) {      // (this, flags, desc filter, ctx, callback)
    int v = this_version(c);
    uint32_t d = heap_alloc(124, 1);
    for (int b = 0; b < 2; ++b)
        for (unsigned i = 0; i < 4; ++i) {
            Surf tmp = {0};
            tmp.w = dd_modes[i].w; tmp.h = dd_modes[i].h;
            pf_display(&tmp, b ? 32 : 16);
            tmp.pitch = tmp.w * tmp.bpp / 8;
            M32(d) = v >= 4 ? 124 : 108;
            write_desc(d, &tmp);
            M32(d + 4) = 0x1000 | 0x2 | 0x4 | 0x8 | 0x40000;   // PIXELFORMAT HEIGHT WIDTH PITCH REFRESHRATE
            M32(d + 24) = 60;
            uint32_t a[2] = {d, arg(c, 3)};
            if (!call_guest(c, arg(c, 4), 2, a)) { heap_free(d); RET(DD_OK, 5); }   // DDENUMRET_CANCEL
        }
    heap_free(d);
    RET(DD_OK, 5);
}
static int dd_get_caps(Cpu *c) {                // (hal caps, hel caps)
    for (int k = 1; k <= 2; ++k) {
        uint32_t p = arg(c, k);
        if (!p) continue;
        uint32_t size = M32(p) ? M32(p) : 380;
        memset(G + p + 4, 0, size - 4);
        M32(p + 4) = 0x1 | 0x40 | 0x200 | 0x400000 | 0x4000000 | 0x80000000;   // 3D BLT BLTSTRETCH COLORKEY BLTCOLORFILL CANBLTSYSMEM
        M32(p + 8) = 0x80000 | 0x200000;                   // CAPS2: CANRENDERWINDOWED, WIDESURFACES
        M32(p + 12) = 0x200;                               // CKEYCAPS: SRCBLT
        M32(p + 56) = 0x400 | 0x200 | 0x100;               // z-buffer depths 16, 24, 32
        M32(p + 60) = 256u << 20; M32(p + 64) = 256u << 20; // 256 MB video memory
        M32(p + 132) = 0x1000 | 0x2000 | 0x200 | 0x4000 | 0x20000 | 0x8 | 0x10;   // ddsOldCaps
    }
    RET(DD_OK, 3);
}
static int dd_get_display_mode(Cpu *c) {
    Surf tmp = {0};
    tmp.w = mode_w ? mode_w : (uint32_t)display_w; tmp.h = mode_h ? mode_h : (uint32_t)display_h;
    pf_display(&tmp, mode_bpp ? mode_bpp : (uint32_t)display_bpp);
    tmp.pitch = tmp.w * tmp.bpp / 8;
    write_desc(arg(c, 1), &tmp);
    M32(arg(c, 1) + 4) = 0x1000 | 0x2 | 0x4 | 0x8 | 0x40000;
    M32(arg(c, 1) + 24) = 60;
    RET(DD_OK, 2);
}
static int dd_set_display_mode(Cpu *c) {        // v1 (w, h, bpp); v2+ (w, h, bpp, refresh, flags)
    mode_w = arg(c, 1); mode_h = arg(c, 2); mode_bpp = arg(c, 3);
    printf("boot: SetDisplayMode(%ux%ux%u)\n", mode_w, mode_h, mode_bpp);
    fflush(stdout);
    RET(DD_OK, this_version(c) == 1 ? 4 : 6);
}
static int dd_restore_display_mode(Cpu *c) { RET(DD_OK, 1); }
static int dd_wait_vblank(Cpu *c) { RET(DD_OK, 3); }
static int dd_get_monitor_frequency(Cpu *c) { M32(arg(c, 1)) = 60; RET(DD_OK, 2); }
static int dd_get_fourcc(Cpu *c) { M32(arg(c, 1)) = 0; RET(DD_OK, 3); }
static int dd_get_vidmem(Cpu *c) {
    if (arg(c, 2)) M32(arg(c, 2)) = 256u << 20;
    if (arg(c, 3)) M32(arg(c, 3)) = 256u << 20;
    RET(DD_OK, 4);
}
static int dd_test_coop(Cpu *c) { RET(DD_OK, 1); }
static int dd_restore_all(Cpu *c) { RET(DD_OK, 1); }
static int dd_flip_to_gdi(Cpu *c) { RET(DD_OK, 1); }
static int dd_get_device_identifier(Cpu *c) {
    uint32_t p = arg(c, 1);
    memset(G + p, 0, 1072);
    snprintf((char *)G + p, 512, "p3mac.drv");
    snprintf((char *)G + p + 512, 512, "Patrician III for Mac (Metal)");
    M32(p + 1024) = 0x00060000; M32(p + 1028) = 0x0004000E;   // driver version 6.0 build 4.14
    M32(p + 1032) = 0x106B; M32(p + 1036) = 0x0001;            // vendor, device
    RET(DD_OK, 3);
}

// ---------------- surfaces ----------------
static unsigned surf_new(uint32_t w, uint32_t h, uint32_t bpp, uint32_t caps) {
    for (unsigned i = 0; i < 4096; ++i)
        if (!surfs[i].used) {
            Surf *s = &surfs[i];
            memset(s, 0, sizeof *s);
            s->used = 1; s->w = w; s->h = h; s->caps = caps;
            pf_display(s, bpp);
            return i;
        }
    die("too many DirectDraw surfaces");
}
static void surf_alloc(Surf *s) {
    s->pitch = ((s->w * (s->bpp ? s->bpp : 16) / 8) + 3) & ~3u;
    s->mem = heap_alloc(s->pitch * s->h + 16, 1);
}

static int dd_create_surface(Cpu *c) {          // (desc, out, outer)
    uint32_t d = arg(c, 1);
    uint32_t flags = M32(d + 4), caps = M32(d + 104);
    uint32_t w = flags & 4 ? M32(d + 12) : 0, h = flags & 2 ? M32(d + 8) : 0;
    int v = this_version(c);
    if (caps & 0x200) {                                    // DDSCAPS_PRIMARYSURFACE
        w = mode_w ? mode_w : (uint32_t)display_w; h = mode_h ? mode_h : (uint32_t)display_h;
    }
    unsigned i = surf_new(w, h, mode_bpp ? mode_bpp : (uint32_t)display_bpp, caps | 0x4000 | 0x8000);   // VIDEOMEMORY LOCALVIDMEM
    Surf *s = &surfs[i];
    // DDSD_PIXELFORMAT: only for off-screen surfaces, and only a filled-in format. The primary surface (and its back
    // buffer) always have the display format; the SGL engine passes the flag with an empty format for it.
    if ((flags & 0x1000) && !(caps & 0x200) && (M32(d + 72 + 4) & ~0x40u ? 1 : M32(d + 72 + 16) | M32(d + 72 + 20) | M32(d + 72 + 24)))
        read_pf(d + 72, s);
    if (caps & 0x20000) {                                  // DDSCAPS_ZBUFFER
        s->pf_flags = 0x400;
        if (flags & 0x1000) s->bpp = M32(d + 84) ? M32(d + 84) : M32(d + 12 + 72);
        if (!(flags & 0x1000)) s->bpp = M32(d + 28) ? M32(d + 28) : 16;   // dwZBufferBitDepth (v1 desc)
        if (!s->bpp) s->bpp = 16;
    }
    if (flags & 0x10000) { s->ck_src_lo = M32(d + 64); s->ck_src_hi = M32(d + 68); s->ck_flags |= 8; }   // DDSD_CKSRCBLT
    if (!s->w || !s->h) die("CreateSurface without a size (flags %X caps %X)", flags, caps);
    surf_alloc(s);
    if (caps & 0x200) {
        primary = i + 1;
        s->caps |= 0x40000 | 0x2000;                       // FRONTBUFFER, VISIBLE
        if ((flags & 0x20) && M32(d + 20)) {               // DDSD_BACKBUFFERCOUNT
            unsigned b = surf_new(s->w, s->h, s->bpp, (caps & ~0x200u) | 0x4 | 0x4000 | 0x8000);   // BACKBUFFER
            surfs[b].pf_flags = s->pf_flags; surfs[b].rmask = s->rmask; surfs[b].gmask = s->gmask; surfs[b].bmask = s->bmask;
            surf_alloc(&surfs[b]);
            s->back = b + 1;
        }
    }
    trace("      CreateSurface(%ux%u, %u bpp, caps %X) -> #%u%s\n", s->w, s->h, s->bpp, s->caps, i, s->back ? " +back buffer" : "");
    M32(arg(c, 2)) = surf_interface(i, v);
    RET(DD_OK, 4);
}

static int surf_query_interface(Cpu *c) {
    uint32_t iid = arg(c, 1), out = arg(c, 2);
    unsigned i = surf_index(com_this(c));
    int v = guid_eq(iid, "6C14DB81-A733-11CE-A521-0020AF0BE560") ? 1 : guid_eq(iid, "57805885-6EEC-11CF-9441-A82303C10E27") ? 2
          : guid_eq(iid, "DA044E00-69B2-11D0-A1D5-00AA00B8DFBB") ? 3 : guid_eq(iid, "0B2B8630-AD35-11D0-8EA6-00609797EA5B") ? 4 : 0;
    if (v) { M32(out) = surf_interface(i, v); RET(DD_OK, 3); }
    if (d3d_query_surface(c, i, iid, out)) RET(DD_OK, 3);
    char s[40];
    guid_str(iid, s, sizeof s);
    fprintf(stderr, "boot: IDirectDrawSurface::QueryInterface(%s): interface not provided yet\n", s);
    M32(out) = 0;
    RET(E_NOINTERFACE, 3);
}
static int surf_get_desc(Cpu *c) { write_desc(arg(c, 1), this_surf(c)); RET(DD_OK, 2); }
static int surf_get_pixel_format(Cpu *c) { write_pf(arg(c, 1), this_surf(c)); RET(DD_OK, 2); }
static int surf_get_caps(Cpu *c) {
    Surf *s = this_surf(c);
    M32(arg(c, 1)) = s->caps;
    if (this_version(c) >= 4) { M32(arg(c, 1) + 4) = 0; M32(arg(c, 1) + 8) = 0; M32(arg(c, 1) + 12) = 0; }
    RET(DD_OK, 2);
}
static int surf_lock(Cpu *c) {                  // (rect, desc, flags, event)
    Surf *s = this_surf(c);
    uint32_t d = arg(c, 2), r = arg(c, 1);
    write_desc(d, s);
    if (r) M32(d + 36) = s->mem + M32(r + 4) * s->pitch + M32(r) * (s->bpp / 8);
    ++s->lock_count;
    RET(DD_OK, 5);
}
static int surf_unlock(Cpu *c) {
    Surf *s = this_surf(c);
    if (s->lock_count) --s->lock_count;
    RET(DD_OK, 2);
}
static int surf_is_lost(Cpu *c) { RET(DD_OK, 1); }
static int surf_restore(Cpu *c) { RET(DD_OK, 1); }
static int surf_page_lock(Cpu *c) { RET(DD_OK, 2); }
static int surf_get_status(Cpu *c) { RET(DD_OK, 2); }
static int surf_set_color_key(Cpu *c) {         // (flags, DDCOLORKEY*)
    Surf *s = this_surf(c);
    if (arg(c, 1) & 8) {                                   // DDCKEY_SRCBLT
        if (arg(c, 2)) { s->ck_src_lo = M32(arg(c, 2)); s->ck_src_hi = M32(arg(c, 2) + 4); s->ck_flags |= 8; }
        else s->ck_flags &= ~8u;
    }
    RET(DD_OK, 3);
}
static int surf_get_color_key(Cpu *c) {
    Surf *s = this_surf(c);
    if (!(s->ck_flags & arg(c, 1))) RET(0x887600E7u, 3);   // DDERR_NOCOLORKEY
    M32(arg(c, 2)) = s->ck_src_lo; M32(arg(c, 2) + 4) = s->ck_src_hi;
    RET(DD_OK, 3);
}
static int surf_set_palette(Cpu *c) { this_surf(c)->palette = arg(c, 1); RET(DD_OK, 2); }
static int surf_get_palette(Cpu *c) {
    Surf *s = this_surf(c);
    if (!s->palette) RET(0x887600E3u, 2);                  // DDERR_NOPALETTEATTACHED
    M32(arg(c, 1)) = s->palette;
    RET(DD_OK, 2);
}
static int surf_set_clipper(Cpu *c) { this_surf(c)->clipper = arg(c, 1); RET(DD_OK, 2); }
static int surf_get_clipper(Cpu *c) {
    Surf *s = this_surf(c);
    if (!s->clipper) RET(0x887600D0u, 2);                  // DDERR_NOCLIPPERATTACHED
    M32(arg(c, 1)) = s->clipper;
    RET(DD_OK, 2);
}
static int surf_get_attached(Cpu *c) {          // (caps*, out)
    Surf *s = this_surf(c);
    uint32_t want = M32(arg(c, 1));
    unsigned i = 0;
    if ((want & 0x4) && s->back) i = s->back;             // BACKBUFFER
    else if ((want & 0x20000) && s->zbuf) i = s->zbuf;    // ZBUFFER
    else if ((want & 0x2000) && s->back && surfs[s->back - 1].zbuf) i = surfs[s->back - 1].zbuf;
    if (!i) { M32(arg(c, 2)) = 0; RET(DDERR_NOTFOUND, 3); }
    M32(arg(c, 2)) = surf_interface(i - 1, this_version(c));
    RET(DD_OK, 3);
}
static int surf_add_attached(Cpu *c) {
    Surf *s = this_surf(c);
    unsigned a = surf_index(arg(c, 1));
    Surf *t = surf_get(a);
    if (t && (t->caps & 0x20000)) {
        s->zbuf = a + 1;
        if (s->back) surfs[s->back - 1].zbuf = a + 1;
    }
    RET(DD_OK, 2);
}
static int surf_delete_attached(Cpu *c) {
    Surf *s = this_surf(c);
    if (surf_index(arg(c, 2)) + 1 == s->zbuf) s->zbuf = 0;
    RET(DD_OK, 3);
}
static int surf_flip(Cpu *c) {
    Surf *s = this_surf(c);
    if (s->back) {
        Surf *b = &surfs[s->back - 1];
        uint32_t m = s->mem; s->mem = b->mem; b->mem = m;
        present(s);
    }
    RET(DD_OK, 3);
}

// ---------------- Blt ----------------
static uint32_t px_get(Surf *s, uint32_t x, uint32_t y) {
    uint32_t a = s->mem + y * s->pitch + x * (s->bpp / 8);
    return s->bpp == 32 ? M32(a) : s->bpp == 16 ? M16(a) : s->bpp == 8 ? M8(a) : (M32(a) & 0xFFFFFF);
}
static void px_set(Surf *s, uint32_t x, uint32_t y, uint32_t v) {
    uint32_t a = s->mem + y * s->pitch + x * (s->bpp / 8);
    if (s->bpp == 32) M32(a) = v; else if (s->bpp == 16) M16(a) = (uint16_t)v; else if (s->bpp == 8) M8(a) = (uint8_t)v;
    else { M16(a) = (uint16_t)v; M8(a + 2) = (uint8_t)(v >> 16); }
}
static void rect_of(uint32_t r, Surf *s, int32_t *x0, int32_t *y0, int32_t *x1, int32_t *y1) {
    if (r) { *x0 = M32(r); *y0 = M32(r + 4); *x1 = M32(r + 8); *y1 = M32(r + 12); }
    else { *x0 = 0; *y0 = 0; *x1 = s->w; *y1 = s->h; }
}
static void blit_rows(Surf *d, int32_t dx0, int32_t dy0, int32_t dx1, int32_t dy1, Surf *s, int32_t sx0, int32_t sy0, int32_t sx1, int32_t sy1,
                      int keyed, uint32_t key_lo, uint32_t key_hi);
// A copy within one surface (the game scrolls its view by shifting the back buffer onto itself) reads from a snapshot
// of the source rows, so overlapping rectangles copy correctly in any direction.
static void blit(Surf *d, int32_t dx0, int32_t dy0, int32_t dx1, int32_t dy1, Surf *s, int32_t sx0, int32_t sy0, int32_t sx1, int32_t sy1,
                 int keyed, uint32_t key_lo, uint32_t key_hi) {
    if (d != s || d->mem != s->mem) { blit_rows(d, dx0, dy0, dx1, dy1, s, sx0, sy0, sx1, sy1, keyed, key_lo, key_hi); return; }
    int32_t y0 = sy0 < 0 ? 0 : sy0, y1 = sy1 > (int32_t)s->h ? (int32_t)s->h : sy1;
    if (y1 <= y0) return;
    Surf tmp = *s;
    tmp.mem = heap_alloc(s->pitch * s->h, 0);
    memcpy(G + tmp.mem + y0 * s->pitch, G + s->mem + y0 * s->pitch, (size_t)(y1 - y0) * s->pitch);
    blit_rows(d, dx0, dy0, dx1, dy1, &tmp, sx0, sy0, sx1, sy1, keyed, key_lo, key_hi);
    heap_free(tmp.mem);
}
static void blit_rows(Surf *d, int32_t dx0, int32_t dy0, int32_t dx1, int32_t dy1, Surf *s, int32_t sx0, int32_t sy0, int32_t sx1, int32_t sy1,
                      int keyed, uint32_t key_lo, uint32_t key_hi) {
    int32_t dw = dx1 - dx0, dh = dy1 - dy0, sw = sx1 - sx0, sh = sy1 - sy0;
    if (dw <= 0 || dh <= 0 || sw <= 0 || sh <= 0) return;
    int same = d->bpp == s->bpp && dw == sw && dh == sh && !keyed;
    for (int32_t y = 0; y < dh; ++y) {
        int32_t ty = dy0 + y, fy = sy0 + (int32_t)((int64_t)y * sh / dh);
        if (ty < 0 || ty >= (int32_t)d->h || fy < 0 || fy >= (int32_t)s->h) continue;
        if (same && dx0 >= 0 && dx1 <= (int32_t)d->w && sx0 >= 0 && sx1 <= (int32_t)s->w) {
            memmove(G + d->mem + ty * d->pitch + dx0 * (d->bpp / 8), G + s->mem + fy * s->pitch + sx0 * (s->bpp / 8), dw * (d->bpp / 8));
            continue;
        }
        for (int32_t x = 0; x < dw; ++x) {
            int32_t tx = dx0 + x, fx = sx0 + (int32_t)((int64_t)x * sw / dw);
            if (tx < 0 || tx >= (int32_t)d->w || fx < 0 || fx >= (int32_t)s->w) continue;
            uint32_t v = px_get(s, fx, fy);
            if (keyed && v >= key_lo && v <= key_hi) continue;
            if (d->bpp != s->bpp) {                        // 16 <-> 32 conversion
                if (s->bpp == 16 && d->bpp == 32) v = ((v >> 11) * 255 / 31) << 16 | (((v >> 5) & 63) * 255 / 63) << 8 | (v & 31) * 255 / 31;
                else if (s->bpp == 32 && d->bpp == 16) v = ((v >> 19) & 31) << 11 | ((v >> 10) & 63) << 5 | ((v >> 3) & 31);
            }
            px_set(d, tx, ty, v);
        }
    }
}
// The last 512 Blt/BltFast calls, for the F12 debug dump.
static struct { uint64_t t; char kind; unsigned dst, src; int32_t dx0, dy0, dx1, dy1, sx0, sy0, sx1, sy1; uint32_t flags; } blts[512];
static unsigned n_blts;
static void blt_log(char kind, unsigned dst, unsigned src, int32_t dx0, int32_t dy0, int32_t dx1, int32_t dy1,
                    int32_t sx0, int32_t sy0, int32_t sx1, int32_t sy1, uint32_t flags) {
    unsigned i = n_blts++ % 512;
    blts[i].t = host_ticks(); blts[i].kind = kind; blts[i].dst = dst; blts[i].src = src; blts[i].flags = flags;
    blts[i].dx0 = dx0; blts[i].dy0 = dy0; blts[i].dx1 = dx1; blts[i].dy1 = dy1;
    blts[i].sx0 = sx0; blts[i].sy0 = sy0; blts[i].sx1 = sx1; blts[i].sy1 = sy1;
}
// F12 in the window: every display-sized surface as PNG, and the recent blits, into user_dir/debug/.
void ddraw_debug_dump(void) {
    char dir[1100], path[1200];
    snprintf(dir, sizeof dir, "%s/debug", user_dir);
    mkdir(dir, 0755);
    for (unsigned i = 0; i < 4096; ++i) {
        Surf *s = surf_get(i);
        if (!s || s->w < 320 || s->h < 200 || (s->bpp != 16 && s->bpp != 32)) continue;
        uint32_t *buf = malloc(4 * s->w * s->h);
        present_rgb(s, buf);
        snprintf(path, sizeof path, "%s/surface_%u_%ux%u_caps%X.png", dir, i, s->w, s->h, s->caps);
        png_write(path, (uint8_t *)buf, s->w, s->h, 4 * s->w);
        free(buf);
    }
    snprintf(path, sizeof path, "%s/blits.txt", dir);
    FILE *f = fopen(path, "w");
    if (f) {
        fprintf(f, "# oldest first: kind dst<-src  dst rect  src rect  flags  ms before the dump\n");
        uint64_t now = host_ticks();
        unsigned start = n_blts > 512 ? n_blts - 512 : 0;
        for (unsigned k = start; k < n_blts; ++k) {
            unsigned i = k % 512;
            fprintf(f, "%c #%u<-#%d  (%d,%d)-(%d,%d)  (%d,%d)-(%d,%d)  %08X  %llu\n", blts[i].kind, blts[i].dst,
                    blts[i].src == 0xFFFF ? -1 : (int)blts[i].src, blts[i].dx0, blts[i].dy0, blts[i].dx1, blts[i].dy1,
                    blts[i].sx0, blts[i].sy0, blts[i].sx1, blts[i].sy1, blts[i].flags,
                    (unsigned long long)((now - blts[i].t) / 1000000));
        }
        fclose(f);
    }
    printf("boot: debug dump -> %s\n", dir);
    fflush(stdout);
}
static int surf_blt(Cpu *c) {                   // (dst rect, src surface, src rect, flags, DDBLTFX*)
    Surf *d = this_surf(c);
    uint32_t flags = arg(c, 4), fx = arg(c, 5);
    int32_t dx0, dy0, dx1, dy1;
    rect_of(arg(c, 1), d, &dx0, &dy0, &dx1, &dy1);
    if (flags & 0x400 || flags & 0x02000000) {             // DDBLT_COLORFILL / DDBLT_DEPTHFILL
        blt_log('F', (unsigned)(d - surfs), 0xFFFF, dx0, dy0, dx1, dy1, 0, 0, 0, 0, flags);
        uint32_t v = M32(fx + 80);
        for (int32_t y = dy0 < 0 ? 0 : dy0; y < dy1 && y < (int32_t)d->h; ++y)
            for (int32_t x = dx0 < 0 ? 0 : dx0; x < dx1 && x < (int32_t)d->w; ++x) px_set(d, x, y, v);
        RET(DD_OK, 6);
    }
    Surf *s = surf_get(surf_index(arg(c, 2)));
    if (!s) RET(DDERR_INVALIDPARAMS, 6);
    int32_t sx0, sy0, sx1, sy1;
    rect_of(arg(c, 3), s, &sx0, &sy0, &sx1, &sy1);
    blt_log('B', (unsigned)(d - surfs), (unsigned)(s - surfs), dx0, dy0, dx1, dy1, sx0, sy0, sx1, sy1, flags);
    int keyed = 0;
    uint32_t lo = 0, hi = 0;
    if (flags & 0x8000) { keyed = 1; lo = s->ck_src_lo; hi = s->ck_src_hi; }                  // DDBLT_KEYSRC
    if (flags & 0x10000) { keyed = 1; lo = M32(fx + 92); hi = M32(fx + 96); }                  // DDBLT_KEYSRCOVERRIDE
    blit(d, dx0, dy0, dx1, dy1, s, sx0, sy0, sx1, sy1, keyed, lo, hi);
    if (d->caps & 0x200) present(d);
    RET(DD_OK, 6);
}
static int surf_blt_fast(Cpu *c) {              // (x, y, src, src rect, trans)
    Surf *d = this_surf(c);
    Surf *s = surf_get(surf_index(arg(c, 3)));
    if (!s) RET(DDERR_INVALIDPARAMS, 6);
    int32_t sx0, sy0, sx1, sy1;
    rect_of(arg(c, 4), s, &sx0, &sy0, &sx1, &sy1);
    int32_t x = arg(c, 1), y = arg(c, 2);
    blt_log('f', (unsigned)(d - surfs), (unsigned)(s - surfs), x, y, x + (sx1 - sx0), y + (sy1 - sy0), sx0, sy0, sx1, sy1, arg(c, 5));
    int keyed = (arg(c, 5) & 1) != 0;                      // DDBLTFAST_SRCCOLORKEY
    blit(d, x, y, x + (sx1 - sx0), y + (sy1 - sy0), s, sx0, sy0, sx1, sy1, keyed, s->ck_src_lo, s->ck_src_hi);
    if (d->caps & 0x200) present(d);
    RET(DD_OK, 6);
}

uint32_t surf_interface(unsigned idx, int version) {
    static uint32_t vt[5];
    if (!vt[version]) {
        static const ComMethod impl[] = {
            {"QueryInterface", surf_query_interface}, {"AddRef", com_addref}, {"Release", com_release},
            {"GetSurfaceDesc", surf_get_desc}, {"GetPixelFormat", surf_get_pixel_format}, {"GetCaps", surf_get_caps},
            {"Lock", surf_lock}, {"Unlock", surf_unlock}, {"IsLost", surf_is_lost}, {"Restore", surf_restore},
            {"PageLock", surf_page_lock}, {"PageUnlock", surf_page_lock}, {"GetBltStatus", surf_get_status},
            {"GetFlipStatus", surf_get_status}, {"SetColorKey", surf_set_color_key}, {"GetColorKey", surf_get_color_key},
            {"SetPalette", surf_set_palette}, {"GetPalette", surf_get_palette}, {"SetClipper", surf_set_clipper},
            {"GetClipper", surf_get_clipper}, {"GetAttachedSurface", surf_get_attached},
            {"AddAttachedSurface", surf_add_attached}, {"DeleteAttachedSurface", surf_delete_attached},
            {"Flip", surf_flip}, {"Blt", surf_blt}, {"BltFast", surf_blt_fast},
        };
        unsigned n = version == 1 ? 36 : version == 2 ? 39 : version == 3 ? 40 : 45;
        char name[32];
        snprintf(name, sizeof name, version == 1 ? "IDirectDrawSurface" : "IDirectDrawSurface%d", version);
        vt[version] = com_vtable(strdup(name), surf_methods, n, impl, sizeof impl / sizeof *impl);
    }
    Surf *s = &surfs[idx];
    if (!s->obj[version]) s->obj[version] = com_new(vt[version], (uint32_t)version << 16 | idx);
    return s->obj[version];
}

// ---------------- clippers and palettes ----------------
static int clipper_set_hwnd(Cpu *c) { M32(com_this(c) + 12) = arg(c, 2); RET(DD_OK, 3); }
static int clipper_get_hwnd(Cpu *c) { M32(arg(c, 1)) = M32(com_this(c) + 12); RET(DD_OK, 2); }
static int clipper_set_clip_list(Cpu *c) { RET(DD_OK, 3); }
static int dd_create_clipper(Cpu *c) {          // (flags, out, outer)
    static uint32_t vt;
    if (!vt) {
        static const ComMethod impl[] = {{"AddRef", com_addref}, {"Release", com_release}, {"SetHWnd", clipper_set_hwnd},
                                         {"GetHWnd", clipper_get_hwnd}, {"SetClipList", clipper_set_clip_list}};
        vt = com_vtable("IDirectDrawClipper", clipper_methods, 9, impl, sizeof impl / sizeof *impl);
    }
    M32(arg(c, 2)) = com_new(vt, 0);
    RET(DD_OK, 4);
}
static int palette_set_entries(Cpu *c) {        // (flags, start, count, entries)
    uint32_t p = com_this(c), table = M32(p + 12);
    memcpy(G + table + 4 * arg(c, 2), G + arg(c, 4), 4 * arg(c, 3));
    RET(DD_OK, 5);
}
static int palette_get_entries(Cpu *c) {
    uint32_t p = com_this(c), table = M32(p + 12);
    memcpy(G + arg(c, 4), G + table + 4 * arg(c, 2), 4 * arg(c, 3));
    RET(DD_OK, 5);
}
static int dd_create_palette(Cpu *c) {          // (flags, entries, out, outer)
    static uint32_t vt;
    if (!vt) {
        static const ComMethod impl[] = {{"AddRef", com_addref}, {"Release", com_release},
                                         {"SetEntries", palette_set_entries}, {"GetEntries", palette_get_entries}};
        vt = com_vtable("IDirectDrawPalette", palette_methods, 7, impl, sizeof impl / sizeof *impl);
    }
    uint32_t o = com_new(vt, 0), table = heap_alloc(1024, 1);
    M32(o + 12) = table;
    if (arg(c, 2)) memcpy(G + table, G + arg(c, 2), 1024);
    M32(arg(c, 3)) = o;
    RET(DD_OK, 5);
}

static uint32_t dd_interface(int version) {
    if (!dd_obj[version]) {
        static const ComMethod impl[] = {
            {"QueryInterface", dd_query_interface}, {"AddRef", com_addref}, {"Release", com_release},
            {"SetCooperativeLevel", dd_set_cooperative_level}, {"EnumDisplayModes", dd_enum_display_modes},
            {"GetCaps", dd_get_caps}, {"GetDisplayMode", dd_get_display_mode}, {"SetDisplayMode", dd_set_display_mode},
            {"RestoreDisplayMode", dd_restore_display_mode}, {"WaitForVerticalBlank", dd_wait_vblank},
            {"GetMonitorFrequency", dd_get_monitor_frequency}, {"GetFourCCCodes", dd_get_fourcc},
            {"GetAvailableVidMem", dd_get_vidmem}, {"TestCooperativeLevel", dd_test_coop},
            {"RestoreAllSurfaces", dd_restore_all}, {"FlipToGDISurface", dd_flip_to_gdi},
            {"GetDeviceIdentifier", dd_get_device_identifier}, {"CreateSurface", dd_create_surface},
            {"CreateClipper", dd_create_clipper}, {"CreatePalette", dd_create_palette},
        };
        unsigned n = version == 1 ? 23 : version == 2 ? 24 : 28;
        char name[32];
        snprintf(name, sizeof name, version == 1 ? "IDirectDraw" : "IDirectDraw%d", version);
        dd_obj[version] = com_new(com_vtable(strdup(name), dd_methods, n, impl, sizeof impl / sizeof *impl), (uint32_t)version << 16);
    }
    return dd_obj[version];
}
uint32_t ddraw_object(int version) { return dd_interface(version); }

#define F(name) if (!strcmp(fn, name))
int win32_ddraw(Cpu *c, const char *fn) {
    F("DirectDrawEnumerateA") {                // (callback, context): one device, the primary display
        static uint32_t desc, name;
        if (!desc) { desc = gstr("DirectDraw HAL"); name = gstr("display"); }
        uint32_t a[4] = {0, desc, name, arg(c, 1)};
        call_guest(c, arg(c, 0), 4, a);
        RET(DD_OK, 2);
    }
    F("DirectDrawCreate") {                    // (guid, out, outer)
        M32(arg(c, 1)) = dd_interface(1);
        RET(DD_OK, 3);
    }
    return 0;
}
