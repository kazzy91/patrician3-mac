// user32.c - USER32 and GDI32, headless for now.
//
// A small window manager in host memory: classes, windows, a posted-message queue, timers, hooks, properties.
// SendMessage calls the window procedure directly (call_guest); DispatchMessage does the same for queued messages.
// CreateWindowEx runs the WH_CBT hook (MFC attaches its CWnd objects there), then WM_NCCREATE and WM_CREATE.
// Nothing is drawn yet: GDI calls succeed without output. The Cocoa/Metal-backed version replaces the drawing and
// input parts later; the bookkeeping stays.
#include <ctype.h>
#include <stdlib.h>
#include <unistd.h>

#include "host.h"

uint32_t find_resource(uint32_t name, uint32_t type);
uint32_t resource_data(uint32_t entry);

#define SCREEN_W 1024
#define SCREEN_H 768

// ---------------- helpers ----------------
#define L(o) ((int32_t)M32((o)))
static void rect_set(uint32_t r, int32_t l, int32_t t, int32_t rt, int32_t b) { M32(r) = l; M32(r + 4) = t; M32(r + 8) = rt; M32(r + 12) = b; }
static int rect_empty(uint32_t r) { return L(r + 8) <= L(r) || L(r + 12) <= L(r + 4); }

static uint32_t wnd_call(Cpu *c, uint32_t proc, uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp) {
    uint32_t a[4] = {hwnd, msg, wp, lp};
    return call_guest(c, proc, 4, a);
}

// ---------------- classes ----------------
typedef struct { int used; char name[64]; uint32_t atom, proc, style, cls_extra, wnd_extra, inst, icon, cursor, brush, menu; } Class;
static Class classes[128];
static uint32_t def_proc_addr;      // host DefWindowProcA, used as the window procedure of system classes

static Class *class_find(uint32_t name_or_atom) {
    for (int i = 0; i < 128; ++i) {
        if (!classes[i].used) continue;
        if (name_or_atom < 0x10000 ? classes[i].atom == name_or_atom : !strcasecmp(classes[i].name, gptr(name_or_atom)))
            return &classes[i];
    }
    return NULL;
}
static Class *class_add(const char *name, uint32_t proc) {
    for (int i = 0; i < 128; ++i)
        if (!classes[i].used) {
            memset(&classes[i], 0, sizeof classes[i]);
            classes[i].used = 1;
            snprintf(classes[i].name, sizeof classes[i].name, "%s", name);
            classes[i].atom = 0xC000 + 0x100 + i;
            classes[i].proc = proc;
            return &classes[i];
        }
    die("too many window classes");
}

// ---------------- windows ----------------
#define HWND_BASE 0x00050000u
#define HWND_DESKTOP_H 0x00010010u
#define NWIN 512
typedef struct { char name[64]; uint32_t value; } Prop;
typedef struct {
    int used, visible, enabled;
    Class *cls;
    uint32_t proc, style, exstyle, parent, owner, id, menu, user, inst;
    int32_t x, y, w, h;
    char text[256];
    uint8_t extra[64];
    Prop props[8];
} Win;
static Win wins[NWIN];
static uint32_t focus, active, capture, foreground;

static Win *win_of(uint32_t h) {
    uint32_t i = h - HWND_BASE;
    return i < NWIN && wins[i].used ? &wins[i] : NULL;
}
static uint32_t hwnd_of(Win *w) { return HWND_BASE + (uint32_t)(w - wins); }
static void screen_origin(Win *w, int32_t *x, int32_t *y) {
    *x = 0; *y = 0;
    for (Win *p = w; p; p = (p->style & 0x40000000) ? win_of(p->parent) : NULL) { *x += p->x; *y += p->y; }   // WS_CHILD
}

// ---------------- message queue, timers, hooks ----------------
typedef struct { uint32_t hwnd, msg, wp, lp; } Msg;
static Msg queue[1024];
static unsigned q_head, q_tail;
static int quit_posted;
static uint32_t quit_code;
typedef struct { int used; uint32_t hwnd, id, proc; uint64_t period, due; } Timer;
static Timer timers[64];
typedef struct { int used; uint32_t type, proc; } Hook;
static Hook hooks[16];
static uint32_t msg_time;
static int show_cursor;

static void post(uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp) {
    if (q_tail - q_head >= 1024) die("message queue full");
    queue[q_tail++ % 1024] = (Msg){hwnd, msg, wp, lp};
}
// Fills msg with the next message (posted messages first, then due timers). Returns 0 if there is none.
static int next_message(uint32_t out, int remove) {
    if (q_head != q_tail) {
        Msg m = queue[q_head % 1024];
        if (remove) ++q_head;
        M32(out) = m.hwnd; M32(out + 4) = m.msg; M32(out + 8) = m.wp; M32(out + 12) = m.lp;
        M32(out + 16) = msg_time = (uint32_t)(host_ticks() / 1000000ull); M32(out + 20) = 0; M32(out + 24) = 0;
        return 1;
    }
    if (quit_posted) {
        if (remove) quit_posted = 0;
        M32(out) = 0; M32(out + 4) = 0x12; M32(out + 8) = quit_code; M32(out + 12) = 0;   // WM_QUIT
        return 1;
    }
    uint64_t now = host_ticks();
    for (int i = 0; i < 64; ++i)
        if (timers[i].used && now >= timers[i].due) {
            if (remove) timers[i].due = now + timers[i].period;
            M32(out) = timers[i].hwnd; M32(out + 4) = 0x113; M32(out + 8) = timers[i].id; M32(out + 12) = timers[i].proc;   // WM_TIMER
            M32(out + 16) = msg_time = (uint32_t)(now / 1000000ull); M32(out + 20) = 0; M32(out + 24) = 0;
            return 1;
        }
    return 0;
}
static uint64_t next_timer_due(void) {
    uint64_t d = UINT64_MAX;
    for (int i = 0; i < 64; ++i) if (timers[i].used && timers[i].due < d) d = timers[i].due;
    return d;
}

static uint32_t send(Cpu *c, uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp) {
    Win *w = win_of(hwnd);
    if (!w) return 0;
    return wnd_call(c, w->proc, hwnd, msg, wp, lp);
}

// ---------------- DefWindowProc (also the procedure of system classes) ----------------
static int def_window_proc(Cpu *c) {
    uint32_t hwnd = arg(c, 0), msg = arg(c, 1), wp = arg(c, 2), lp = arg(c, 3);
    Win *w = win_of(hwnd);
    (void)wp;
    switch (msg) {
    case 0x81: RET(1, 4);                                  // WM_NCCREATE
    case 0x0C: if (w) snprintf(w->text, sizeof w->text, "%s", lp ? gptr(lp) : ""); RET(1, 4);   // WM_SETTEXT
    case 0x0D: if (!w || !wp) RET(0, 4); snprintf((char *)G + lp, wp, "%s", w->text); RET(strlen((char *)G + lp), 4);   // WM_GETTEXT
    case 0x0E: RET(w ? strlen(w->text) : 0, 4);            // WM_GETTEXTLENGTH
    case 0x10: if (w) { uint32_t a[4] = {hwnd, 0x02, 0, 0}; (void)a; } RET(0, 4);   // WM_CLOSE: the app decides
    case 0x84: RET(1, 4);                                  // WM_NCHITTEST: HTCLIENT
    case 0x20: RET(0, 4);                                  // WM_SETCURSOR
    case 0x7F: RET(0, 4);                                  // WM_GETICON
    default: RET(0, 4);
    }
}

static uint32_t create_window(Cpu *c, uint32_t exstyle, uint32_t cls_name, uint32_t title, uint32_t style, int32_t x, int32_t y,
                              int32_t cw, int32_t ch, uint32_t parent, uint32_t menu, uint32_t inst, uint32_t param) {
    Class *cls = class_find(cls_name);
    if (!cls) {
        const char *n = cls_name < 0x10000 ? "?" : gptr(cls_name);
        trace("      CreateWindowExA: unknown class %s, using a static window\n", n);
        cls = class_add(cls_name < 0x10000 ? "#atom" : n, def_proc_addr);
    }
    int i = 0;
    while (i < NWIN && wins[i].used) ++i;
    if (i == NWIN) die("too many windows");
    Win *w = &wins[i];
    memset(w, 0, sizeof *w);
    w->used = 1; w->enabled = !(style & 0x08000000);
    w->cls = cls; w->proc = cls->proc; w->style = style; w->exstyle = exstyle; w->inst = inst;
    if (style & 0x40000000) { w->parent = parent; w->id = menu; } else { w->owner = parent; w->menu = menu; }
    if (x == (int32_t)0x80000000) x = 0;                   // CW_USEDEFAULT
    if (y == (int32_t)0x80000000) y = 0;
    if (cw == (int32_t)0x80000000) cw = SCREEN_W;
    if (ch == (int32_t)0x80000000) ch = SCREEN_H;
    w->x = x; w->y = y; w->w = cw; w->h = ch;
    snprintf(w->text, sizeof w->text, "%s", title ? (title < 0x10000 ? "" : gptr(title)) : "");
    uint32_t hwnd = hwnd_of(w);
    trace("      CreateWindowExA(class %s, \"%s\", style %08X, %d,%d %dx%d, parent %08X) -> %08X\n",
          cls->name, w->text, style, x, y, cw, ch, parent, hwnd);
    // CREATESTRUCTA on the guest stack area of the heap
    uint32_t cs = heap_alloc(48, 1);
    M32(cs) = param; M32(cs + 4) = inst; M32(cs + 8) = menu; M32(cs + 12) = parent;
    M32(cs + 16) = ch; M32(cs + 20) = cw; M32(cs + 24) = y; M32(cs + 28) = x;
    M32(cs + 32) = style; M32(cs + 36) = title; M32(cs + 40) = cls_name; M32(cs + 44) = exstyle;
    for (int k = 0; k < 16; ++k)
        if (hooks[k].used && hooks[k].type == 5) {         // WH_CBT: HCBT_CREATEWND
            uint32_t cbt = heap_alloc(8, 1);
            M32(cbt) = cs; M32(cbt + 4) = 0;
            uint32_t a[3] = {3, hwnd, cbt};
            uint32_t r = call_guest(c, hooks[k].proc, 3, a);
            heap_free(cbt);
            if (r) { w->used = 0; heap_free(cs); return 0; }
        }
    uint32_t r = wnd_call(c, w->proc, hwnd, 0x81, 0, cs);                 // WM_NCCREATE
    if (!r) { trace("      WM_NCCREATE refused\n"); w->used = 0; heap_free(cs); return 0; }
    r = wnd_call(c, w->proc, hwnd, 0x01, 0, cs);                          // WM_CREATE
    if (r == 0xFFFFFFFF) { trace("      WM_CREATE refused\n"); w->used = 0; heap_free(cs); return 0; }
    heap_free(cs);
    if (w->used) {
        wnd_call(c, w->proc, hwnd, 0x05, 0, (uint32_t)(w->h << 16 | (w->w & 0xFFFF)));   // WM_SIZE
        wnd_call(c, w->proc, hwnd, 0x03, 0, (uint32_t)(w->y << 16 | (w->x & 0xFFFF)));   // WM_MOVE
        if (style & 0x10000000) w->visible = 1;           // WS_VISIBLE
    }
    return hwnd;
}

// Property names are strings or atoms.
static void prop_name(uint32_t a, char *n, size_t sz) {
    if (a < 0x10000) snprintf(n, sz, "#%u", a);
    else snprintf(n, sz, "%s", gptr(a));
}

// ---------------- wsprintf ----------------
static int format(char *out, size_t n, const char *fmt, uint32_t args) {
    size_t o = 0;
    char tmp[512];
    for (const char *p = fmt; *p; ++p) {
        if (*p != '%') { if (o + 1 < n) out[o++] = *p; continue; }
        char spec[32];
        int s = 0;
        spec[s++] = *p++;
        while (*p && strchr("-+ #0123456789.", *p) && s < 24) spec[s++] = *p++;
        int is_long = 0;
        while (*p == 'l' || *p == 'h') { is_long = 1; ++p; }
        (void)is_long;
        char conv = *p;
        if (!conv) break;
        if (conv == '%') { if (o + 1 < n) out[o++] = '%'; continue; }
        if (conv == 'i') conv = 'd';
        spec[s++] = conv; spec[s] = 0;
        int k;
        if (conv == 's') { uint32_t a = M32(args); args += 4; k = snprintf(tmp, sizeof tmp, spec, a ? gptr(a) : "(null)"); }
        else if (conv == 'c') { uint32_t a = M32(args); args += 4; k = snprintf(tmp, sizeof tmp, spec, (int)(uint8_t)a); }
        else if (strchr("duxXo", conv)) { uint32_t a = M32(args); args += 4; k = conv == 'd' ? snprintf(tmp, sizeof tmp, spec, (int32_t)a) : snprintf(tmp, sizeof tmp, spec, a); }
        else die("wsprintf: unsupported conversion %%%c in \"%s\"", conv, fmt);
        for (int j = 0; j < k && j < (int)sizeof tmp - 1 && o + 1 < n; ++j) out[o++] = tmp[j];
    }
    out[o] = 0;
    return (int)o;
}

// ---------------- the import table ----------------
#define F(name) if (!strcmp(fn, name))
int win32_user32(Cpu *c, const char *fn) {
    if (!def_proc_addr) {
        def_proc_addr = host_fn(def_window_proc, "DefWindowProcA(system class)");
        static const char *sys[] = {"Button", "Edit", "Static", "ListBox", "ComboBox", "ScrollBar", "#32770", "MDIClient"};
        for (unsigned i = 0; i < sizeof sys / sizeof *sys; ++i) class_add(sys[i], def_proc_addr);
    }
    // ---- rectangles and points ----
    F("SetRect") { rect_set(arg(c, 0), arg(c, 1), arg(c, 2), arg(c, 3), arg(c, 4)); RET(1, 5); }
    F("SetRectEmpty") { rect_set(arg(c, 0), 0, 0, 0, 0); RET(1, 1); }
    F("CopyRect") { memmove(G + arg(c, 0), G + arg(c, 1), 16); RET(1, 2); }
    F("IsRectEmpty") RET(rect_empty(arg(c, 0)), 1);
    F("EqualRect") RET(!memcmp(G + arg(c, 0), G + arg(c, 1), 16), 2);
    F("PtInRect") {
        uint32_t r = arg(c, 0);
        int32_t x = arg(c, 1), y = arg(c, 2);
        RET(x >= L(r) && x < L(r + 8) && y >= L(r + 4) && y < L(r + 12), 3);
    }
    F("OffsetRect") {
        uint32_t r = arg(c, 0);
        int32_t dx = arg(c, 1), dy = arg(c, 2);
        rect_set(r, L(r) + dx, L(r + 4) + dy, L(r + 8) + dx, L(r + 12) + dy);
        RET(1, 3);
    }
    F("InflateRect") {
        uint32_t r = arg(c, 0);
        int32_t dx = arg(c, 1), dy = arg(c, 2);
        rect_set(r, L(r) - dx, L(r + 4) - dy, L(r + 8) + dx, L(r + 12) + dy);
        RET(1, 3);
    }
    F("IntersectRect") {
        uint32_t d = arg(c, 0), a = arg(c, 1), b = arg(c, 2);
        int32_t l = L(a) > L(b) ? L(a) : L(b), t = L(a + 4) > L(b + 4) ? L(a + 4) : L(b + 4);
        int32_t r = L(a + 8) < L(b + 8) ? L(a + 8) : L(b + 8), bt = L(a + 12) < L(b + 12) ? L(a + 12) : L(b + 12);
        if (l >= r || t >= bt) { rect_set(d, 0, 0, 0, 0); RET(0, 3); }
        rect_set(d, l, t, r, bt);
        RET(1, 3);
    }
    F("UnionRect") {
        uint32_t d = arg(c, 0), a = arg(c, 1), b = arg(c, 2);
        if (rect_empty(a)) { memmove(G + d, G + b, 16); RET(!rect_empty(b), 3); }
        if (rect_empty(b)) { memmove(G + d, G + a, 16); RET(1, 3); }
        int32_t l = L(a) < L(b) ? L(a) : L(b), t = L(a + 4) < L(b + 4) ? L(a + 4) : L(b + 4);
        int32_t r = L(a + 8) > L(b + 8) ? L(a + 8) : L(b + 8), bt = L(a + 12) > L(b + 12) ? L(a + 12) : L(b + 12);
        rect_set(d, l, t, r, bt);
        RET(1, 3);
    }
    F("SubtractRect") { memmove(G + arg(c, 0), G + arg(c, 1), 16); RET(!rect_empty(arg(c, 0)), 3); }
    F("CharUpperA") {
        uint32_t p = arg(c, 0);
        if (p < 0x10000) RET(toupper(p & 0xFF), 1);
        for (char *s = (char *)G + p; *s; ++s) *s = toupper((unsigned char)*s);
        RET(p, 1);
    }
    F("wsprintfA") {                    // cdecl (buf, fmt, ...)
        char out[1024];
        int k = format(out, sizeof out, gptr(arg(c, 1)), c->esp + 12);
        memcpy(G + arg(c, 0), out, k + 1);
        RET_CDECL(k);
    }
    F("wvsprintfA") {                   // (buf, fmt, va_list)
        char out[1024];
        int k = format(out, sizeof out, gptr(arg(c, 1)), arg(c, 2));
        memcpy(G + arg(c, 0), out, k + 1);
        RET(k, 3);
    }

    // ---- system ----
    F("GetSystemMetrics") {
        switch (arg(c, 0)) {
        case 0: case 16: case 78: RET(SCREEN_W, 1);        // SM_CXSCREEN, SM_CXFULLSCREEN, SM_CXVIRTUALSCREEN
        case 1: case 17: case 79: RET(SCREEN_H, 1);
        case 2: case 3: RET(16, 1);                        // scroll bar sizes
        case 4: RET(19, 1);                                // SM_CYCAPTION
        case 5: case 6: RET(1, 1);                         // borders
        case 7: case 8: RET(3, 1);                         // dialog frames
        case 11: case 12: RET(32, 1);                      // icons
        case 13: case 14: RET(32, 1);                      // cursors
        case 15: RET(19, 1);                               // SM_CYMENU
        case 19: RET(1, 1);                                // SM_MOUSEPRESENT
        case 32: case 33: RET(4, 1);                       // SM_CXFRAME
        case 43: RET(3, 1);                                // SM_CMOUSEBUTTONS
        case 49: case 50: RET(16, 1);                      // small icons
        case 80: RET(1, 1);                                // SM_CMONITORS
        default: RET(0, 1);
        }
    }
    F("GetSysColor") RET(0x00C0C0C0, 1);
    F("GetSysColorBrush") RET(0x00080000 + arg(c, 0), 1);
    F("SystemParametersInfoA") {
        if (arg(c, 0) == 48 && arg(c, 2)) rect_set(arg(c, 2), 0, 0, SCREEN_W, SCREEN_H);   // SPI_GETWORKAREA
        else if (arg(c, 2) && arg(c, 0) < 0x1000 && (arg(c, 0) & 1) == 0) M32(arg(c, 2)) = 0;
        RET(1, 4);
    }
    F("GetKeyState") RET(0, 1);
    F("GetKeyboardState") { memset(G + arg(c, 0), 0, 256); RET(1, 1); }
    F("ToAscii") RET(0, 5);
    F("GetCursorPos") { M32(arg(c, 0)) = SCREEN_W / 2; M32(arg(c, 0) + 4) = SCREEN_H / 2; RET(1, 1); }
    F("SetCursorPos") RET(1, 2);
    F("SetCursor") RET(0, 1);
    F("ShowCursor") { show_cursor += arg(c, 0) ? 1 : -1; RET(show_cursor, 1); }
    F("MessageBoxA") {
        fprintf(stderr, "boot: MessageBox \"%s\": %s\n", arg(c, 2) ? gptr(arg(c, 2)) : "", arg(c, 1) ? gptr(arg(c, 1)) : "");
        RET(1, 4);                                         // IDOK
    }
    F("RegisterWindowMessageA") {
        static uint32_t next = 0xC200;
        RET(next++, 1);
    }
    F("WinHelpA") RET(1, 4);

    // ---- resources ----
    F("LoadStringA") {                                     // (inst, id, buf, cap)
        uint32_t id = arg(c, 1), buf = arg(c, 2);
        int cap = (int32_t)arg(c, 3);
        uint32_t e = find_resource(id / 16 + 1, 6);       // RT_STRING
        if (!e || cap <= 0) { if (cap > 0) M8(buf) = 0; RET(0, 4); }
        uint32_t p = resource_data(e);
        for (unsigned k = 0; k < id % 16; ++k) p += 2 + 2 * M16(p);
        unsigned n = M16(p);
        unsigned k = 0;
        for (; k < n && (int)k < cap - 1; ++k) { uint16_t ch = M16(p + 2 + 2 * k); M8(buf + k) = ch < 256 ? (uint8_t)ch : '?'; }
        M8(buf + k) = 0;
        RET(k, 4);
    }
    F("LoadCursorA") RET(0x00060000 + (arg(c, 1) & 0xFFFF), 2);
    F("LoadIconA") RET(0x00061000 + (arg(c, 1) & 0xFFF), 2);
    F("LoadBitmapA") RET(0x00062000 + (arg(c, 1) & 0xFFF), 2);
    F("LoadMenuA") RET(0x00063000 + (arg(c, 1) & 0xFFF), 2);
    F("LoadAcceleratorsA") RET(0x00064000 + (arg(c, 1) & 0xFFF), 2);
    F("TranslateAcceleratorA") RET(0, 3);

    // ---- menus (no menus are shown yet) ----
    F("GetMenu") { Win *w = win_of(arg(c, 0)); RET(w ? w->menu : 0, 1); }
    F("SetMenu") { Win *w = win_of(arg(c, 0)); if (w) w->menu = arg(c, 1); RET(1, 2); }
    F("DestroyMenu") RET(1, 1);
    F("GetSubMenu") RET(0, 2);
    F("GetMenuItemCount") RET(0, 1);
    F("GetMenuItemID") RET(0xFFFFFFFF, 2);
    F("GetMenuState") RET(0xFFFFFFFF, 3);
    F("CheckMenuItem") RET(0xFFFFFFFF, 3);
    F("EnableMenuItem") RET(0xFFFFFFFF, 3);
    F("ModifyMenuA") RET(1, 5);
    F("SetMenuItemBitmaps") RET(1, 5);
    F("GetMenuCheckMarkDimensions") RET(13 << 16 | 13, 0);

    // ---- classes ----
    F("RegisterClassA") {                                  // WNDCLASSA
        uint32_t wc = arg(c, 0);
        const char *name = M32(wc + 36) < 0x10000 ? "#atom" : gptr(M32(wc + 36));
        Class *k = class_find(M32(wc + 36));
        if (!k) k = class_add(name, M32(wc + 4));
        k->style = M32(wc); k->proc = M32(wc + 4); k->cls_extra = M32(wc + 8); k->wnd_extra = M32(wc + 12);
        k->inst = M32(wc + 16); k->icon = M32(wc + 20); k->cursor = M32(wc + 24); k->brush = M32(wc + 28); k->menu = M32(wc + 32);
        trace("      RegisterClassA(\"%s\", proc %08X)\n", name, k->proc);
        RET(k->atom, 1);
    }
    F("UnregisterClassA") { Class *k = class_find(arg(c, 0)); if (k) k->used = 0; RET(1, 2); }
    F("GetClassInfoA") {                                   // (inst, name, WNDCLASSA*)
        Class *k = class_find(arg(c, 1));
        if (!k) { last_error = 1411; RET(0, 3); }
        uint32_t wc = arg(c, 2);
        M32(wc) = k->style; M32(wc + 4) = k->proc; M32(wc + 8) = k->cls_extra; M32(wc + 12) = k->wnd_extra;
        M32(wc + 16) = k->inst; M32(wc + 20) = k->icon; M32(wc + 24) = k->cursor; M32(wc + 28) = k->brush;
        M32(wc + 32) = k->menu; M32(wc + 36) = arg(c, 1);
        RET(k->atom, 3);
    }
    F("GetClassLongA") {
        Win *w = win_of(arg(c, 0));
        if (!w) RET(0, 2);
        switch ((int32_t)arg(c, 1)) {
        case -24: RET(w->cls->proc, 2);                    // GCL_WNDPROC
        case -26: RET(w->cls->style, 2);                   // GCL_STYLE
        case -12: RET(w->cls->cursor, 2);                  // GCL_HCURSOR
        case -14: RET(w->cls->icon, 2);                    // GCL_HICON
        case -10: RET(w->cls->brush, 2);                   // GCL_HBRBACKGROUND
        case -32: RET(w->cls->atom, 2);                    // GCW_ATOM
        default: RET(0, 2);
        }
    }
    F("GetClassNameA") {
        Win *w = win_of(arg(c, 0));
        if (!w) RET(0, 3);
        snprintf((char *)G + arg(c, 1), arg(c, 2), "%s", w->cls->name);
        RET(strlen((char *)G + arg(c, 1)), 3);
    }

    // ---- windows ----
    F("CreateWindowExA") {
        uint32_t h = create_window(c, arg(c, 0), arg(c, 1), arg(c, 2), arg(c, 3), arg(c, 4), arg(c, 5), arg(c, 6),
                                   arg(c, 7), arg(c, 8), arg(c, 9), arg(c, 10), arg(c, 11));
        RET(h, 12);
    }
    F("DefWindowProcA") return def_window_proc(c);
    F("CallWindowProcA") {                                 // (proc, hwnd, msg, wp, lp)
        uint32_t r = wnd_call(c, arg(c, 0), arg(c, 1), arg(c, 2), arg(c, 3), arg(c, 4));
        RET(r, 5);
    }
    F("DestroyWindow") {
        Win *w = win_of(arg(c, 0));
        if (!w) RET(0, 1);
        uint32_t h = arg(c, 0);
        for (int i = 0; i < NWIN; ++i)
            if (wins[i].used && (wins[i].parent == h || wins[i].owner == h)) {
                uint32_t a[1] = {hwnd_of(&wins[i])};
                (void)a;
                send(c, hwnd_of(&wins[i]), 0x02, 0, 0);
                send(c, hwnd_of(&wins[i]), 0x82, 0, 0);
                wins[i].used = 0;
            }
        send(c, h, 0x02, 0, 0);                            // WM_DESTROY
        send(c, h, 0x82, 0, 0);                            // WM_NCDESTROY
        if ((w = win_of(h))) w->used = 0;
        if (focus == h) focus = 0;
        if (active == h) active = 0;
        RET(1, 1);
    }
    F("IsWindow") RET(win_of(arg(c, 0)) != NULL, 1);
    F("IsWindowVisible") { Win *w = win_of(arg(c, 0)); RET(w && w->visible, 1); }
    F("IsWindowEnabled") { Win *w = win_of(arg(c, 0)); RET(w && w->enabled, 1); }
    F("EnableWindow") { Win *w = win_of(arg(c, 0)); int was = w ? !w->enabled : 0; if (w) w->enabled = arg(c, 1) != 0; RET(was, 2); }
    F("IsIconic") RET(0, 1);
    F("ShowWindow") {
        Win *w = win_of(arg(c, 0));
        int was = w && w->visible;
        if (w) {
            int vis = arg(c, 1) != 0;                      // SW_HIDE = 0
            if (vis != was) { w->visible = vis; send(c, arg(c, 0), 0x18, vis, 0); }   // WM_SHOWWINDOW
        }
        RET(was, 2);
    }
    F("UpdateWindow") RET(1, 1);
    F("ShowOwnedPopups") RET(1, 2);
    F("BringWindowToTop") RET(1, 1);
    F("MoveWindow") {
        Win *w = win_of(arg(c, 0));
        if (!w) RET(0, 6);
        w->x = arg(c, 1); w->y = arg(c, 2); w->w = arg(c, 3); w->h = arg(c, 4);
        send(c, arg(c, 0), 0x05, 0, (uint32_t)(w->h << 16 | (w->w & 0xFFFF)));
        RET(1, 6);
    }
    F("SetWindowPos") {                                    // (hwnd, after, x, y, cx, cy, flags)
        Win *w = win_of(arg(c, 0));
        uint32_t fl = arg(c, 6);
        if (!w) RET(0, 7);
        if (!(fl & 2)) { w->x = arg(c, 2); w->y = arg(c, 3); }   // SWP_NOMOVE
        if (!(fl & 1)) { w->w = arg(c, 4); w->h = arg(c, 5); send(c, arg(c, 0), 0x05, 0, (uint32_t)(w->h << 16 | (w->w & 0xFFFF))); }
        if (fl & 0x40) w->visible = 1;                     // SWP_SHOWWINDOW
        if (fl & 0x80) w->visible = 0;                     // SWP_HIDEWINDOW
        RET(1, 7);
    }
    F("BeginDeferWindowPos") RET(0x00070000, 1);
    F("DeferWindowPos") {                                  // (hdwp, hwnd, after, x, y, cx, cy, flags)
        Win *w = win_of(arg(c, 1));
        uint32_t fl = arg(c, 7);
        if (w) {
            if (!(fl & 2)) { w->x = arg(c, 3); w->y = arg(c, 4); }
            if (!(fl & 1)) { w->w = arg(c, 5); w->h = arg(c, 6); }
        }
        RET(arg(c, 0), 8);
    }
    F("EndDeferWindowPos") RET(1, 1);
    F("GetWindowRect") {
        Win *w = win_of(arg(c, 0));
        if (!w) { if (arg(c, 0) == HWND_DESKTOP_H) { rect_set(arg(c, 1), 0, 0, SCREEN_W, SCREEN_H); RET(1, 2); } RET(0, 2); }
        int32_t x, y;
        screen_origin(w, &x, &y);
        rect_set(arg(c, 1), x, y, x + w->w, y + w->h);
        RET(1, 2);
    }
    F("GetClientRect") {
        Win *w = win_of(arg(c, 0));
        if (!w) { rect_set(arg(c, 1), 0, 0, SCREEN_W, SCREEN_H); RET(arg(c, 0) == HWND_DESKTOP_H, 2); }
        rect_set(arg(c, 1), 0, 0, w->w, w->h);
        RET(1, 2);
    }
    F("GetWindowPlacement") {
        Win *w = win_of(arg(c, 0));
        uint32_t p = arg(c, 1);
        memset(G + p + 4, 0, 40);
        M32(p + 8) = w && w->visible ? 1 : 0;              // showCmd
        if (w) rect_set(p + 28, w->x, w->y, w->x + w->w, w->y + w->h);
        RET(w != NULL, 2);
    }
    F("AdjustWindowRectEx") RET(1, 4);
    F("ClientToScreen") {
        Win *w = win_of(arg(c, 0));
        int32_t x = 0, y = 0;
        if (w) screen_origin(w, &x, &y);
        M32(arg(c, 1)) += x; M32(arg(c, 1) + 4) += y;
        RET(1, 2);
    }
    F("ScreenToClient") {
        Win *w = win_of(arg(c, 0));
        int32_t x = 0, y = 0;
        if (w) screen_origin(w, &x, &y);
        M32(arg(c, 1)) -= x; M32(arg(c, 1) + 4) -= y;
        RET(1, 2);
    }
    F("MapWindowPoints") {                                 // (from, to, pts, n)
        int32_t fx = 0, fy = 0, tx = 0, ty = 0;
        Win *a = win_of(arg(c, 0)), *b = win_of(arg(c, 1));
        if (a) screen_origin(a, &fx, &fy);
        if (b) screen_origin(b, &tx, &ty);
        for (uint32_t i = 0; i < arg(c, 3); ++i) { M32(arg(c, 2) + 8 * i) += fx - tx; M32(arg(c, 2) + 8 * i + 4) += fy - ty; }
        RET((uint32_t)((fy - ty) << 16 | ((fx - tx) & 0xFFFF)), 4);
    }
    F("GetWindowLongA") {
        Win *w = win_of(arg(c, 0));
        if (!w) RET(0, 2);
        int32_t i = arg(c, 1);
        switch (i) {
        case -4: RET(w->proc, 2);
        case -6: RET(w->inst, 2);
        case -8: RET(w->owner, 2);
        case -12: RET(w->id, 2);
        case -16: RET(w->style, 2);
        case -20: RET(w->exstyle, 2);
        case -21: RET(w->user, 2);
        default: RET(i >= 0 && i + 4 <= 64 ? *(uint32_t *)(w->extra + i) : 0, 2);
        }
    }
    F("SetWindowLongA") {
        Win *w = win_of(arg(c, 0));
        if (!w) RET(0, 3);
        int32_t i = arg(c, 1);
        uint32_t v = arg(c, 2), old = 0;
        switch (i) {
        case -4: old = w->proc; w->proc = v; break;
        case -6: old = w->inst; w->inst = v; break;
        case -8: old = w->owner; w->owner = v; break;
        case -12: old = w->id; w->id = v; break;
        case -16: old = w->style; w->style = v; break;
        case -20: old = w->exstyle; w->exstyle = v; break;
        case -21: old = w->user; w->user = v; break;
        default: if (i >= 0 && i + 4 <= 64) { old = *(uint32_t *)(w->extra + i); *(uint32_t *)(w->extra + i) = v; }
        }
        RET(old, 3);
    }
    F("GetParent") { Win *w = win_of(arg(c, 0)); RET(w ? (w->style & 0x40000000 ? w->parent : w->owner) : 0, 1); }
    F("GetDesktopWindow") RET(HWND_DESKTOP_H, 0);
    F("GetTopWindow") {
        for (int i = 0; i < NWIN; ++i) if (wins[i].used && wins[i].parent == arg(c, 0) && (wins[i].style & 0x40000000)) RET(hwnd_of(&wins[i]), 1);
        RET(0, 1);
    }
    F("GetWindow") {                                       // (hwnd, cmd)
        uint32_t h = arg(c, 0);
        Win *w = win_of(h);
        switch (arg(c, 1)) {
        case 4: RET(w ? w->owner : 0, 2);                  // GW_OWNER
        case 5:                                            // GW_CHILD
            for (int i = 0; i < NWIN; ++i) if (wins[i].used && wins[i].parent == h && (wins[i].style & 0x40000000)) RET(hwnd_of(&wins[i]), 2);
            RET(0, 2);
        case 2:                                            // GW_HWNDNEXT: next sibling
            if (!w) RET(0, 2);
            for (int i = (int)(w - wins) + 1; i < NWIN; ++i)
                if (wins[i].used && wins[i].parent == w->parent && (wins[i].style & 0x40000000) == (w->style & 0x40000000)) RET(hwnd_of(&wins[i]), 2);
            RET(0, 2);
        default: RET(0, 2);
        }
    }
    F("GetLastActivePopup") RET(arg(c, 0), 1);
    F("GetDlgCtrlID") { Win *w = win_of(arg(c, 0)); RET(w ? w->id : 0, 1); }
    F("GetDlgItem") {
        for (int i = 0; i < NWIN; ++i) if (wins[i].used && wins[i].parent == arg(c, 0) && wins[i].id == arg(c, 1)) RET(hwnd_of(&wins[i]), 2);
        RET(0, 2);
    }
    F("GetNextDlgTabItem") RET(0, 3);
    F("SendDlgItemMessageA") {
        uint32_t h = 0;
        for (int i = 0; i < NWIN && !h; ++i) if (wins[i].used && wins[i].parent == arg(c, 0) && wins[i].id == arg(c, 1)) h = hwnd_of(&wins[i]);
        uint32_t r = send(c, h, arg(c, 2), arg(c, 3), arg(c, 4));
        RET(r, 5);
    }
    F("SetWindowTextA") { uint32_t r = send(c, arg(c, 0), 0x0C, 0, arg(c, 1)); RET(r, 2); }
    F("GetWindowTextA") { uint32_t r = send(c, arg(c, 0), 0x0D, arg(c, 2), arg(c, 1)); RET(r, 3); }
    F("GetWindowTextLengthA") { uint32_t r = send(c, arg(c, 0), 0x0E, 0, 0); RET(r, 1); }
    F("FindWindowA") RET(0, 2);
    F("GetActiveWindow") RET(active, 0);
    F("SetActiveWindow") { uint32_t o = active; active = arg(c, 0); RET(o, 1); }
    F("GetForegroundWindow") RET(foreground, 0);
    F("SetForegroundWindow") { foreground = active = arg(c, 0); RET(1, 1); }
    F("GetFocus") RET(focus, 0);
    F("SetFocus") { uint32_t o = focus; focus = arg(c, 0); RET(o, 1); }
    F("GetCapture") RET(capture, 0);
    F("SetCapture") { uint32_t o = capture; capture = arg(c, 0); RET(o, 1); }
    F("ReleaseCapture") { capture = 0; RET(1, 0); }
    F("GetPropA") {
        Win *w = win_of(arg(c, 0));
        char n[64];
        prop_name(arg(c, 1), n, sizeof n);
        if (w) for (int i = 0; i < 8; ++i) if (!strcmp(w->props[i].name, n)) RET(w->props[i].value, 2);
        RET(0, 2);
    }
    F("SetPropA") {
        Win *w = win_of(arg(c, 0));
        char n[64];
        prop_name(arg(c, 1), n, sizeof n);
        if (!w) RET(0, 3);
        for (int i = 0; i < 8; ++i) if (!strcmp(w->props[i].name, n)) { w->props[i].value = arg(c, 2); RET(1, 3); }
        for (int i = 0; i < 8; ++i) if (!w->props[i].name[0]) { snprintf(w->props[i].name, 64, "%s", n); w->props[i].value = arg(c, 2); RET(1, 3); }
        RET(0, 3);
    }
    F("RemovePropA") {
        Win *w = win_of(arg(c, 0));
        char n[64];
        prop_name(arg(c, 1), n, sizeof n);
        if (w) for (int i = 0; i < 8; ++i) if (!strcmp(w->props[i].name, n)) { uint32_t v = w->props[i].value; w->props[i].name[0] = 0; RET(v, 2); }
        RET(0, 2);
    }

    // ---- messages ----
    F("SendMessageA") { uint32_t r = send(c, arg(c, 0), arg(c, 1), arg(c, 2), arg(c, 3)); RET(r, 4); }
    F("PostMessageA") {
        if (arg(c, 0) == 0xFFFF) RET(1, 4);                // HWND_BROADCAST
        post(arg(c, 0), arg(c, 1), arg(c, 2), arg(c, 3));
        RET(1, 4);
    }
    F("PostQuitMessage") { quit_posted = 1; quit_code = arg(c, 0); RET(0, 1); }
    F("PeekMessageA") {                                    // (msg, hwnd, min, max, flags)
        RET(next_message(arg(c, 0), arg(c, 4) & 1), 5);
    }
    F("GetMessageA") {
        for (;;) {
            if (next_message(arg(c, 0), 1)) RET(M32(arg(c, 0) + 4) != 0x12, 4);
            uint64_t due = next_timer_due();
            if (due == UINT64_MAX) {
                printf("boot: message loop idle: GetMessageA with no messages and no timers after %u import calls\n", import_calls);
                exit(0);
            }
            uint64_t now = host_ticks();
            if (due > now) usleep((useconds_t)((due - now) / 1000));
        }
    }
    F("TranslateMessage") RET(0, 1);
    F("DispatchMessageA") {
        uint32_t m = arg(c, 0);
        if (M32(m + 4) == 0x113 && M32(m + 12)) {          // WM_TIMER with a TIMERPROC
            uint32_t a[4] = {M32(m), 0x113, M32(m + 8), (uint32_t)(host_ticks() / 1000000ull)};
            call_guest(c, M32(m + 12), 4, a);
            RET(0, 1);
        }
        uint32_t r = send(c, M32(m), M32(m + 4), M32(m + 8), M32(m + 12));
        RET(r, 1);
    }
    F("IsDialogMessageA") RET(0, 2);
    F("GetMessagePos") RET((SCREEN_H / 2) << 16 | SCREEN_W / 2, 0);
    F("GetMessageTime") RET(msg_time, 0);
    F("ReuseDDElParam") RET(arg(c, 4) << 16 | (arg(c, 3) & 0xFFFF), 5);
    F("UnpackDDElParam") { if (arg(c, 2)) M32(arg(c, 2)) = arg(c, 1) & 0xFFFF; if (arg(c, 3)) M32(arg(c, 3)) = arg(c, 1) >> 16; RET(1, 4); }
    F("SetTimer") {                                        // (hwnd, id, ms, proc)
        uint32_t id = arg(c, 1);
        for (int i = 0; i < 64; ++i)
            if (timers[i].used && timers[i].hwnd == arg(c, 0) && timers[i].id == id) { timers[i].used = 0; break; }
        for (int i = 0; i < 64; ++i)
            if (!timers[i].used) {
                if (!arg(c, 0) && !id) id = 0x7F00 + i;
                uint64_t p = (uint64_t)(arg(c, 2) ? arg(c, 2) : 1) * 1000000ull;
                timers[i] = (Timer){1, arg(c, 0), id, arg(c, 3), p, host_ticks() + p};
                trace("      SetTimer(%08X, %u, %u ms, proc %08X)\n", arg(c, 0), id, arg(c, 2), arg(c, 3));
                RET(id ? id : 1, 4);
            }
        RET(0, 4);
    }
    F("KillTimer") {
        for (int i = 0; i < 64; ++i) if (timers[i].used && timers[i].hwnd == arg(c, 0) && timers[i].id == arg(c, 1)) { timers[i].used = 0; RET(1, 2); }
        RET(0, 2);
    }
    F("SetWindowsHookExA") {                               // (type, proc, inst, thread)
        for (int i = 0; i < 16; ++i)
            if (!hooks[i].used) { hooks[i] = (Hook){1, arg(c, 0), arg(c, 1)}; trace("      SetWindowsHookExA(%d, %08X)\n", arg(c, 0), arg(c, 1)); RET(0x00090000 + i, 4); }
        RET(0, 4);
    }
    F("UnhookWindowsHookEx") { uint32_t i = arg(c, 0) - 0x00090000; if (i < 16) hooks[i].used = 0; RET(1, 1); }
    F("CallNextHookEx") RET(0, 4);

    // ---- painting (nothing is drawn yet) ----
    F("GetDC") RET(0x000A0000 + (arg(c, 0) & 0xFFFF), 1);
    F("ReleaseDC") RET(1, 2);
    F("BeginPaint") {
        uint32_t ps = arg(c, 1);
        memset(G + ps, 0, 64);
        M32(ps) = 0x000A0000 + (arg(c, 0) & 0xFFFF);
        Win *w = win_of(arg(c, 0));
        if (w) rect_set(ps + 8, 0, 0, w->w, w->h);
        RET(M32(ps), 2);
    }
    F("EndPaint") RET(1, 2);
    F("InvalidateRect") RET(1, 3);
    F("ValidateRect") RET(1, 2);
    F("DrawTextA") RET(16, 5);
    F("TabbedTextOutA") RET(0, 8);
    F("GrayStringA") RET(1, 9);

    // ---- dialogs: not yet ----
    F("EndDialog") RET(1, 2);
    return 0;
}

int win32_gdi32(Cpu *c, const char *fn) {
    F("GetStockObject") RET(0x000B0000 + arg(c, 0), 1);
    F("GetDeviceCaps") {
        switch (arg(c, 1)) {
        case 8: RET(SCREEN_W, 2);                          // HORZRES
        case 10: RET(SCREEN_H, 2);                         // VERTRES
        case 12: RET(16, 2);                               // BITSPIXEL
        case 14: RET(1, 2);                                // PLANES
        case 88: case 90: RET(96, 2);                      // LOGPIXELSX/Y
        case 4: RET(320, 2);                               // HORZSIZE (mm)
        case 6: RET(240, 2);                               // VERTSIZE
        case 24: RET(-1, 2);                               // NUMCOLORS
        case 38: RET(0x7E99, 2);                           // RASTERCAPS
        default: RET(0, 2);
        }
    }
    F("SelectObject") RET(0x000B0000, 2);
    F("DeleteObject") RET(1, 1);
    F("DeleteDC") RET(1, 1);
    F("SaveDC") RET(1, 1);
    F("RestoreDC") RET(1, 2);
    F("SetBkColor") RET(0xFFFFFF, 2);
    F("SetTextColor") RET(0, 2);
    F("SetMapMode") RET(1, 2);
    F("SetViewportOrgEx") { if (arg(c, 3)) { M32(arg(c, 3)) = 0; M32(arg(c, 3) + 4) = 0; } RET(1, 4); }
    F("OffsetViewportOrgEx") { if (arg(c, 3)) { M32(arg(c, 3)) = 0; M32(arg(c, 3) + 4) = 0; } RET(1, 4); }
    F("SetViewportExtEx") { if (arg(c, 3)) { M32(arg(c, 3)) = 1; M32(arg(c, 3) + 4) = 1; } RET(1, 4); }
    F("SetWindowExtEx") { if (arg(c, 3)) { M32(arg(c, 3)) = 1; M32(arg(c, 3) + 4) = 1; } RET(1, 4); }
    F("ScaleViewportExtEx") RET(1, 6);
    F("ScaleWindowExtEx") RET(1, 6);
    F("TextOutA") RET(1, 5);
    F("ExtTextOutA") RET(1, 8);
    F("MoveToEx") RET(1, 4);
    F("LineTo") RET(1, 3);
    F("GetClipBox") { rect_set(arg(c, 1), 0, 0, SCREEN_W, SCREEN_H); RET(2, 2); }   // SIMPLEREGION
    F("PtVisible") RET(1, 3);
    F("RectVisible") RET(1, 2);
    F("Escape") RET(0, 5);
    F("CreateBitmap") RET(0x000C0000, 5);
    F("GetObjectA") { memset(G + arg(c, 2), 0, arg(c, 1)); RET(0, 3); }
    return 0;
}
