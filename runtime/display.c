// display.c - the display the game sees.
//
// The game supports 800x600, 1024x768 and 1280x1024, each at 16 or 32 bits (its own mode table). By default the port
// runs it at the largest, 1280x1024x32; the Mac window later scales that to the screen, keeping the 5:4 shape.
// P3_RESOLUTION=WxH[xBPP] picks another mode. The choice is written into the user folder's copies of the game's
// settings (P2.cfg [OPTIONS] AUFLOESUNG/FARBTIEFE, game.ini [SGL] Size/ColDepth) unless the user already set them,
// and the hardware test results in the registry (registry.c) already mark every mode "OK", so the video test that
// P3Setup.exe runs on Windows is skipped.
#include <stdlib.h>

#include "host.h"

int ini_get(const char *file, const char *sec, const char *key, char *out, size_t n);
int ini_set(const char *file, const char *sec, const char *key, const char *val);

static const struct { int w, h; } modes[] = {{800, 600}, {1024, 768}, {1280, 1024}};
int display_w = 1280, display_h = 1024, display_bpp = 32;

void display_init(void) {
    const char *r = getenv("P3_RESOLUTION");
    if (r) {
        int w = 0, h = 0, b = 32;
        if (sscanf(r, "%dx%dx%d", &w, &h, &b) >= 2) {
            int ok = 0;
            for (unsigned i = 0; i < 3; ++i) ok |= modes[i].w == w && modes[i].h == h;
            if (!ok || (b != 16 && b != 32)) die("P3_RESOLUTION=%s: the game supports 800x600, 1024x768 and 1280x1024 at 16 or 32 bits", r);
            display_w = w; display_h = h; display_bpp = b;
        }
    }
    unsigned idx = display_w == 800 ? 0 : display_w == 1024 ? 1 : 2;
    char v[64], cur[64];
    // P2.cfg holds the in-game options: AUFLOESUNG = mode index, FARBTIEFE = 0 for 16 bits, 1 for 32.
    snprintf(v, sizeof v, "%u", idx);
    if (r || !ini_get("P2.cfg", "OPTIONS", "AUFLOESUNG", cur, sizeof cur) || strcmp(cur, v)) {
        char host[2048];
        path_resolve("P2.cfg", host, sizeof host, 0);
        if (r || !strstr(host, user_dir)) {                // not chosen by the user in this port yet
            ini_set("P2.cfg", "OPTIONS", "AUFLOESUNG", v);
            ini_set("P2.cfg", "OPTIONS", "FARBTIEFE", display_bpp == 32 ? "1" : "0");
        }
    }
    if (r || !ini_get("game.ini", "SGL", "Size", cur, sizeof cur)) {
        snprintf(v, sizeof v, "%d %d", display_w, display_h);
        ini_set("game.ini", "SGL", "Size", v);
        snprintf(v, sizeof v, "%d", display_bpp);
        ini_set("game.ini", "SGL", "ColDepth", v);
    }
    printf("boot: display %dx%dx%d\n", display_w, display_h, display_bpp);
}

// DEVMODEA: bits per pixel at 104, width 108, height 112, flags 116, frequency 120.
static void fill_devmode(uint32_t p, int w, int h, int bpp) {
    M16(p + 36) = 124;                                     // dmSize
    M32(p + 40) = 0x5C0000;                                // DM_BITSPERPEL | DM_PELSWIDTH | DM_PELSHEIGHT | DM_DISPLAYFREQUENCY
    M32(p + 104) = bpp; M32(p + 108) = w; M32(p + 112) = h; M32(p + 116) = 0; M32(p + 120) = 60;
}

#define F(name) if (!strcmp(fn, name))
int win32_display(Cpu *c, const char *fn) {
    F("EnumDisplaySettingsA") {                            // (device, index, DEVMODEA*)
        int32_t i = arg(c, 1);
        if (i == -1 || i == -2) { fill_devmode(arg(c, 2), display_w, display_h, display_bpp); RET(1, 3); }   // current / registry
        if (i < 0 || i >= 6) RET(0, 3);
        fill_devmode(arg(c, 2), modes[i % 3].w, modes[i % 3].h, i < 3 ? 16 : 32);
        RET(1, 3);
    }
    F("ChangeDisplaySettingsA") {                          // (DEVMODEA*, flags)
        uint32_t p = arg(c, 0);
        if (p) trace("      ChangeDisplaySettingsA(%ux%ux%u)\n", M32(p + 108), M32(p + 112), M32(p + 104));
        RET(0, 2);                                         // DISP_CHANGE_SUCCESSFUL
    }
    return 0;
}
