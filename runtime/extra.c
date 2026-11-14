// extra.c - the rest of the imported Win32 surface: functions the game's modules import (mostly through statically
// linked MFC and the C runtime) that need little more than a plausible answer. Real behaviour lives in the other
// files; this one keeps them short. Drawing functions succeed without drawing (the game draws its own UI with
// DirectDraw and bitmap fonts).
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#include "host.h"

uint32_t find_resource(uint32_t module, uint32_t name, uint32_t type);
uint32_t resource_size(uint32_t entry);

static void systemtime(uint32_t p, time_t t, int local) {
    struct tm tm;
    if (local) localtime_r(&t, &tm); else gmtime_r(&t, &tm);
    M16(p) = tm.tm_year + 1900; M16(p + 2) = tm.tm_mon + 1; M16(p + 4) = tm.tm_wday; M16(p + 6) = tm.tm_mday;
    M16(p + 8) = tm.tm_hour; M16(p + 10) = tm.tm_min; M16(p + 12) = tm.tm_sec; M16(p + 14) = 0;
}

#define F(name) if (!strcmp(fn, name))
static int kernel32(Cpu *c, const char *fn) {
    F("GetUserDefaultLCID") RET(0x409, 0);
    F("GetThreadLocale") RET(0x409, 0);
    F("IsValidCodePage") RET(1, 1);
    F("IsValidLocale") RET(1, 2);
    F("EnumSystemLocalesA") RET(1, 2);
    F("GetLocaleInfoA") {                      // (lcid, type, buf, cap)
        const char *v;
        switch (arg(c, 1) & 0xFFFF) {
        case 0x0B: v = "850"; break;           // LOCALE_IDEFAULTCODEPAGE
        case 0x1004: v = "1252"; break;        // LOCALE_IDEFAULTANSICODEPAGE
        case 0x0E: v = "."; break;             // LOCALE_SDECIMAL
        case 0x0F: v = ","; break;             // LOCALE_STHOUSAND
        case 0x01: v = "0409"; break;          // LOCALE_ILANGUAGE
        case 0x07: v = "ENU"; break;           // LOCALE_SABBREVCTRYNAME
        case 0x03: v = "ENU"; break;           // LOCALE_SABBREVLANGNAME
        default: v = ""; break;
        }
        if (!arg(c, 3)) RET(strlen(v) + 1, 4);
        snprintf((char *)G + arg(c, 2), arg(c, 3), "%s", v);
        RET(strlen(v) + 1, 4);
    }
    F("GetLocaleInfoW") {
        if (!arg(c, 3)) RET(1, 4);
        M16(arg(c, 2)) = 0;
        RET(1, 4);
    }
    F("GetStringTypeExA") {                    // (lcid, type, src, n, out): as GetStringTypeA
        uint32_t src = arg(c, 2), out = arg(c, 4);
        int n = (int32_t)arg(c, 3);
        if (n < 0) n = (int)strlen(gptr(src)) + 1;
        for (int i = 0; i < n; ++i) {
            uint8_t b = M8(src + i);
            uint16_t t = (isupper(b) ? 1 : 0) | (islower(b) ? 2 : 0) | (isdigit(b) ? 4 : 0) | (isspace(b) ? 8 : 0) |
                         (ispunct(b) ? 0x10 : 0) | (iscntrl(b) ? 0x20 : 0) | (isxdigit(b) ? 0x80 : 0) | (isalpha(b) ? 0x100 : 0);
            M16(out + 2 * i) = t;
        }
        RET(1, 5);
    }
    F("GetSystemTime") { systemtime(arg(c, 0), time(NULL), 0); RET(0, 1); }
    F("SystemTimeToFileTime") {
        uint32_t p = arg(c, 0);
        struct tm tm = {0};
        tm.tm_year = M16(p) - 1900; tm.tm_mon = M16(p + 2) - 1; tm.tm_mday = M16(p + 6);
        tm.tm_hour = M16(p + 8); tm.tm_min = M16(p + 10); tm.tm_sec = M16(p + 12);
        time_t t = timegm(&tm);
        M64(arg(c, 1)) = (uint64_t)t * 10000000ull + 116444736000000000ull + M16(p + 14) * 10000ull;
        RET(1, 2);
    }
    F("LocalFileTimeToFileTime") { M64(arg(c, 1)) = M64(arg(c, 0)); RET(1, 2); }
    F("GetTempPathA") {
        const char *s = "C:\\Patrician3\\temp\\";
        snprintf((char *)G + arg(c, 1), arg(c, 0), "%s", s);
        RET(strlen(s), 2);
    }
    F("GetTempFileNameA") {                    // (dir, prefix, unique, out)
        static unsigned n = 1;
        unsigned u = arg(c, 2) ? arg(c, 2) : n++;
        snprintf((char *)G + arg(c, 3), 260, "%s\\%.3s%04X.tmp", gptr(arg(c, 0)), arg(c, 1) ? gptr(arg(c, 1)) : "", u & 0xFFFF);
        RET(u, 4);
    }
    F("GetShortPathNameA") {
        snprintf((char *)G + arg(c, 1), arg(c, 2), "%s", gptr(arg(c, 0)));
        RET(strlen(gptr(arg(c, 0))), 3);
    }
    F("SetFileAttributesA") RET(1, 2);
    F("SetFileTime") RET(1, 4);
    F("MoveFileA") {
        char from[2048], to[2048];
        path_resolve(gptr(arg(c, 0)), from, sizeof from, 0);
        path_resolve(gptr(arg(c, 1)), to, sizeof to, 1);
        if (!strstr(from, user_dir)) { last_error = 5; RET(0, 2); }   // never move files out of the game folder
        RET(rename(from, to) == 0, 2);
    }
    F("FatalAppExitA") die("FatalAppExit: %s", gptr(arg(c, 1)));
    F("SetConsoleCtrlHandler") RET(1, 2);
    F("SizeofResource") RET(resource_size(arg(c, 1)), 2);
    return 0;
}

static int user32(Cpu *c, const char *fn) {
    F("IsChild") {
        // walk up from the child (GetParent semantics through GWL_HWNDPARENT is enough here)
        RET(0, 2);
    }
    F("GetWindowDC") RET(0x000A0000 + (arg(c, 0) & 0xFFFF), 1);
    F("CharToOemA") { if (arg(c, 0) != arg(c, 1)) strcpy((char *)G + arg(c, 1), gptr(arg(c, 0))); RET(1, 2); }
    F("OemToCharA") { if (arg(c, 0) != arg(c, 1)) strcpy((char *)G + arg(c, 1), gptr(arg(c, 0))); RET(1, 2); }
    F("GetDialogBaseUnits") RET(16 << 16 | 8, 0);
    F("CheckDlgButton") RET(1, 3);
    F("CheckRadioButton") RET(1, 4);
    F("IsDlgButtonChecked") RET(0, 2);
    F("GetDlgItemInt") { if (arg(c, 2)) M32(arg(c, 2)) = 0; RET(0, 4); }
    F("SetDlgItemInt") RET(1, 4);
    F("GetDlgItemTextA") { if (arg(c, 3)) M8(arg(c, 2)) = 0; RET(0, 4); }
    F("SetDlgItemTextA") RET(1, 3);
    F("GetScrollInfo") RET(0, 3);
    F("SetScrollInfo") RET(0, 4);
    F("GetScrollPos") RET(0, 2);
    F("SetScrollPos") RET(0, 4);
    F("GetScrollRange") { M32(arg(c, 2)) = 0; M32(arg(c, 3)) = 0; RET(1, 4); }
    F("SetScrollRange") RET(1, 5);
    F("ShowScrollBar") RET(1, 3);
    F("ScrollWindow") RET(1, 5);
    F("ScrollWindowEx") RET(1, 8);
    F("SetWindowPlacement") RET(1, 2);
    F("DeleteMenu") RET(1, 3);
    F("InsertMenuA") RET(1, 5);
    F("GetMenuStringA") { if (arg(c, 3)) M8(arg(c, 2)) = 0; RET(0, 5); }
    F("TrackPopupMenu") RET(0, 7);
    F("CreateDialogIndirectParamA") die("CreateDialogIndirectParamA: dialogs are not implemented yet (called from %08X)", M32(c->esp));
    return 0;
}

static int gdi32(Cpu *c, const char *fn) {
    static uint32_t next_handle = 0x000D0000;
    F("CreateSolidBrush") RET(++next_handle, 1);
    F("CreateHatchBrush") RET(++next_handle, 2);
    F("CreatePatternBrush") RET(++next_handle, 1);
    F("CreateDIBPatternBrushPt") RET(++next_handle, 2);
    F("CreatePen") RET(++next_handle, 3);
    F("ExtCreatePen") RET(++next_handle, 5);
    F("CreateFontIndirectA") RET(++next_handle, 1);
    F("CreateRectRgn") RET(++next_handle, 4);
    F("GetObjectType") RET(0, 1);
    F("GetTextExtentPoint32A") { M32(arg(c, 3)) = 8 * arg(c, 2); M32(arg(c, 3) + 4) = 16; RET(1, 4); }
    F("GetTextMetricsA") {
        uint32_t p = arg(c, 1);
        memset(G + p, 0, 56);
        M32(p) = 16; M32(p + 4) = 13; M32(p + 8) = 3;      // height, ascent, descent
        M32(p + 20) = 8; M32(p + 24) = 16; M32(p + 28) = 400;   // average and max width, weight
        M8(p + 45) = ' '; M8(p + 46) = 0xFF; M8(p + 47) = '?'; M8(p + 48) = ' ';
        RET(1, 2);
    }
    F("GetCurrentPositionEx") { M32(arg(c, 1)) = 0; M32(arg(c, 1) + 4) = 0; RET(1, 2); }
    F("GetDCOrgEx") { M32(arg(c, 1)) = 0; M32(arg(c, 1) + 4) = 0; RET(1, 2); }
    F("GetViewportExtEx") { M32(arg(c, 1)) = 1; M32(arg(c, 1) + 4) = 1; RET(1, 2); }
    F("GetWindowExtEx") { M32(arg(c, 1)) = 1; M32(arg(c, 1) + 4) = 1; RET(1, 2); }
    F("SetWindowOrgEx") { if (arg(c, 3)) { M32(arg(c, 3)) = 0; M32(arg(c, 3) + 4) = 0; } RET(1, 4); }
    F("OffsetWindowOrgEx") { if (arg(c, 3)) { M32(arg(c, 3)) = 0; M32(arg(c, 3) + 4) = 0; } RET(1, 4); }
    F("GetClipRgn") RET(0, 2);
    F("SelectClipRgn") RET(2, 2);
    F("ExtSelectClipRgn") RET(2, 3);
    F("SelectClipPath") RET(1, 2);
    F("OffsetClipRgn") RET(2, 3);
    F("ExcludeClipRect") RET(2, 5);
    F("IntersectClipRect") RET(2, 5);
    F("SelectPalette") RET(0x000B0000, 3);
    F("SetBkMode") RET(1, 2);
    F("SetPolyFillMode") RET(1, 2);
    F("SetROP2") RET(13, 2);
    F("SetStretchBltMode") RET(1, 2);
    F("SetTextAlign") RET(0, 2);
    F("SetTextCharacterExtra") RET(0, 2);
    F("SetTextJustification") RET(1, 3);
    F("SetMapperFlags") RET(0, 2);
    F("SetArcDirection") RET(1, 2);
    F("SetColorAdjustment") RET(1, 2);
    F("PlayMetaFile") RET(1, 2);
    F("PlayMetaFileRecord") RET(1, 4);
    F("EnumMetaFile") RET(1, 4);
    F("ArcTo") RET(1, 9);
    F("PolyBezierTo") RET(1, 3);
    F("PolyDraw") RET(1, 4);
    F("PolylineTo") RET(1, 3);
    F("StartDocA") RET(0, 2);
    return 0;
}

// ---------------- VERSION.dll: the RT_VERSION resource of a loaded module ----------------
#include "module.h"
uint32_t resource_data(uint32_t entry);
static uint32_t version_resource(const char *path) {
    const ModuleTables *m = module_by_name(path);
    return m ? find_resource(m->base, 1, 16) : 0;          // VS_VERSION_INFO, RT_VERSION
}
// VS_VERSIONINFO node: wLength, wValueLength, wType, szKey (UTF-16), value (4-aligned), children (4-aligned).
static int key_eq(uint32_t k, const char *s, size_t n) {
    for (size_t i = 0; i < n; ++i) if (tolower(M16(k + 2 * i) & 0xFF) != tolower((unsigned char)s[i]) || M16(k + 2 * i) > 0xFF) return 0;
    return M16(k + 2 * n) == 0;
}
static uint32_t node_value(uint32_t p) {
    uint32_t k = p + 6;
    while (M16(k)) k += 2;
    return (k + 2 + 3) & ~3u;
}
static uint32_t node_children(uint32_t p) {
    uint32_t v = node_value(p);
    return (v + M16(p + 2) * (M16(p + 4) == 1 ? 2 : 1) + 3) & ~3u;
}
static int ver_query(uint32_t block, const char *path, uint32_t *ptr, uint32_t *len) {
    uint32_t node = block;
    const char *s = path;
    while (*s == '\\') ++s;
    while (*s) {
        const char *e = strchr(s, '\\');
        size_t n = e ? (size_t)(e - s) : strlen(s);
        uint32_t ch = node_children(node), end = node + M16(node);
        uint32_t found = 0;
        while (ch + 6 < end && M16(ch)) {
            if (key_eq(ch + 6, s, n)) { found = ch; break; }
            ch = (ch + M16(ch) + 3) & ~3u;
        }
        if (!found) return 0;
        node = found;
        s += n;
        while (*s == '\\') ++s;
    }
    uint32_t v = node_value(node), vl = M16(node + 2);
    if (M16(node + 4) == 1 && node != block) {             // a text value: hand out an ANSI copy
        uint32_t a = heap_alloc(vl + 1, 1);
        for (uint32_t i = 0; i < vl; ++i) { uint16_t w = M16(v + 2 * i); M8(a + i) = w < 256 ? (uint8_t)w : '?'; }
        *ptr = a; *len = vl;
        return 1;
    }
    *ptr = v; *len = vl;
    return vl != 0;
}

int win32_extra(Cpu *c, const char *dll, const char *fn) {
    if (!strcasecmp(dll, "KERNEL32.dll")) return kernel32(c, fn);
    if (!strcasecmp(dll, "USER32.dll")) return user32(c, fn);
    if (!strcasecmp(dll, "GDI32.dll")) return gdi32(c, fn);
    if (!strcasecmp(dll, "ADVAPI32.dll")) {
        F("RegDeleteKeyA") RET(0, 2);
        F("RegDeleteValueA") RET(0, 2);
        F("RegOpenKeyA") {                     // (root, sub, out) -> RegOpenKeyExA
            uint32_t sp = c->esp, root = arg(c, 0), sub = arg(c, 1), out = arg(c, 2), ra = M32(c->esp);
            c->esp -= 8;                       // re-push as (root, sub, 0, KEY_ALL_ACCESS, out)
            M32(c->esp) = ra; M32(c->esp + 4) = root; M32(c->esp + 8) = sub; M32(c->esp + 12) = 0;
            M32(c->esp + 16) = 0xF003F; M32(c->esp + 20) = out;
            int r = win32_advapi(c, "RegOpenKeyExA");
            c->esp = sp + 16;                  // stdcall, 3 arguments
            return r;
        }
        return 0;
    }
    if (!strcasecmp(dll, "SHELL32.dll")) {
        F("DragAcceptFiles") RET(0, 2);
        F("DragFinish") RET(0, 1);
        F("DragQueryFileA") RET(0, 4);
        F("SHGetFileInfoA") RET(0, 5);
        F("ShellExecuteA") RET(2, 6);          // SE_ERR_FNF: nothing is launched
        return 0;
    }
    if (!strcasecmp(dll, "VERSION.dll")) {
        F("GetFileVersionInfoSizeA") {         // (path, handle*)
            uint32_t e = version_resource(gptr(arg(c, 0)));
            if (arg(c, 1)) M32(arg(c, 1)) = 0;
            if (!e) { last_error = 1813; RET(0, 2); }      // ERROR_RESOURCE_TYPE_NOT_FOUND
            RET(resource_size(e), 2);
        }
        F("GetFileVersionInfoA") {             // (path, handle, size, out)
            uint32_t e = version_resource(gptr(arg(c, 0)));
            if (!e) { last_error = 1813; RET(0, 4); }
            uint32_t n = resource_size(e) < arg(c, 2) ? resource_size(e) : arg(c, 2);
            memcpy(G + arg(c, 3), G + resource_data(e), n);
            RET(1, 4);
        }
        F("VerQueryValueA") {                  // (block, sub-block, out ptr*, out len*)
            uint32_t ptr = 0, len = 0;
            int ok = ver_query(arg(c, 0), gptr(arg(c, 1)), &ptr, &len);
            if (ok) { M32(arg(c, 2)) = ptr; M32(arg(c, 3)) = len; }
            RET(ok, 4);
        }
        return 0;
    }
    if (!strcasecmp(dll, "WINSPOOL.DRV")) {
        F("OpenPrinterA") { if (arg(c, 1)) M32(arg(c, 1)) = 0; RET(0, 3); }
        F("ClosePrinter") RET(1, 1);
        F("DocumentPropertiesA") RET(0xFFFFFFFF, 6);
        return 0;
    }
    return 0;
}
