// input.c - mouse and keyboard state, and the window messages the game gets for them.
//
// Sources: the Mac window (window.m) and a script (P3_INPUT, for tests without a window). Positions are in game
// pixels (the DirectDraw display mode); the window converts from its own scaled coordinates. Everything runs on the
// main thread: events are taken in input_pump(), which the game's own PeekMessage / GetMessage / Sleep calls reach.
//
// P3_INPUT is a list of "seconds:action" separated by ';', times counted from start:
//     move X Y | down X Y | up X Y | click X Y | rdown X Y | rup X Y | rclick X Y | key VK | char C | close | quit
// (close = what closing the window or Cmd-Q does: WM_CLOSE to the game; quit = stop at once)
// e.g. P3_INPUT="4:click 300 250;8:key 27;12:quit"   (VK in decimal or 0x hex; click = move, press, release)
#include <pthread.h>
#include <stdlib.h>

#include "host.h"

void user32_post_mouse(uint32_t msg, uint32_t wp, int x, int y);   // user32.c: to the window under the point (or capture)
void user32_post_key(uint32_t msg, uint32_t wp, uint32_t lp);      // user32.c: to the focus window
void user32_request_close(void);                                    // user32.c: WM_CLOSE to the main window
void window_pump(void);                                             // window.m
void ddraw_display_size(int *w, int *h);                            // ddraw.c: the current display mode

static uint8_t keys[256];              // 0x80: down, 0x01: toggled (as GetKeyboardState)
static int mouse_x, mouse_y;
static uint64_t close_requested_at;

static uint32_t mk_flags(void) {       // MK_LBUTTON, MK_RBUTTON, MK_SHIFT, MK_CONTROL, MK_MBUTTON
    return (keys[1] >> 7) | (keys[2] >> 7) << 1 | (keys[0x10] >> 7) << 2 | (keys[0x11] >> 7) << 3 | (keys[4] >> 7) << 4;
}
static void set_key(int vk, int down) {
    if (down && !(keys[vk] & 0x80)) keys[vk] ^= 1;
    keys[vk] = (keys[vk] & 1) | (down ? 0x80 : 0);
}

void input_mouse_move(int x, int y) {
    int w, h;
    ddraw_display_size(&w, &h);
    mouse_x = x < 0 ? 0 : x >= w ? w - 1 : x;
    mouse_y = y < 0 ? 0 : y >= h ? h - 1 : y;
    user32_post_mouse(0x200, mk_flags(), mouse_x, mouse_y);          // WM_MOUSEMOVE
}
// button: 0 left, 1 right, 2 middle
void input_mouse_button(int button, int down) {
    static const int vk[3] = {1, 2, 4};
    static const uint32_t msg[3][2] = {{0x202, 0x201}, {0x205, 0x204}, {0x208, 0x207}};   // up, down
    if (button < 0 || button > 2) return;
    set_key(vk[button], down);
    user32_post_mouse(msg[button][down != 0], mk_flags(), mouse_x, mouse_y);
}
void input_mouse_wheel(int delta) {    // multiples of 120, positive = away from the user
    user32_post_mouse(0x20A, (uint32_t)delta << 16 | mk_flags(), mouse_x, mouse_y);
}
// vk: Windows virtual key; ch: the character it types (0 for none), sent as WM_CHAR after the key-down.
void input_key(int vk, int down, int ch) {
    if (vk > 0 && vk < 256) {
        int was = keys[vk] & 0x80;
        set_key(vk, down);
        uint32_t lp = 1u | (down ? (was ? 0x40000000u : 0) : 0xC0000000u);
        user32_post_key(down ? 0x100 : 0x101, (uint32_t)vk, lp);     // WM_KEYDOWN / WM_KEYUP
    }
    if (down && ch > 0 && ch < 256) user32_post_key(0x102, (uint32_t)ch, 1);   // WM_CHAR
}
void input_request_close(void) {
    if (close_requested_at) { printf("boot: closed by the user (second request)\n"); exit(0); }
    close_requested_at = host_ticks();
    user32_request_close();
}
void input_cursor_pos(int *x, int *y) { *x = mouse_x; *y = mouse_y; }

// ---------------- script ----------------
typedef struct { uint64_t t; int kind, a, b; } Step;    // kind: 0 move, 1 button(a) down(b), 2 key, 3 char, 4 quit, 5 close
static Step steps[256];
static unsigned n_steps, next_step;
static uint64_t t0;
static void add(double t, int kind, int a, int b) {
    if (n_steps < 256) steps[n_steps++] = (Step){(uint64_t)(t * 1e9), kind, a, b};
}
static int step_cmp(const void *a, const void *b) {
    uint64_t x = ((const Step *)a)->t, y = ((const Step *)b)->t;
    return x < y ? -1 : x > y;
}
int input_scripted(void) { return next_step < n_steps || close_requested_at; }

void input_init(void) {
    t0 = host_ticks();
    int w, h;
    ddraw_display_size(&w, &h);
    mouse_x = w / 2; mouse_y = h / 2;
    const char *s = getenv("P3_INPUT");
    while (s && *s) {
        char act[16] = "";
        double t = 0;
        int a = 0, b = 0, used = 0;
        if (sscanf(s, " %lf : %15s%n", &t, act, &used) < 2) die("P3_INPUT: cannot read \"%.30s\"", s);
        const char *rest = s + used;
        char *end = (char *)rest;
        a = (int)strtol(rest, &end, 0);
        if (end != rest) { rest = end; b = (int)strtol(rest, &end, 0); }
        int button = act[0] == 'r' ? 1 : 0;
        const char *base = act[0] == 'r' ? act + 1 : act;
        if (!strcmp(act, "move")) add(t, 0, a, b);
        else if (!strcmp(base, "down")) { add(t, 0, a, b); add(t + 0.02, 1, button, 1); }
        else if (!strcmp(base, "up")) { add(t, 0, a, b); add(t + 0.02, 1, button, 0); }
        else if (!strcmp(base, "click")) { add(t, 0, a, b); add(t + 0.15, 1, button, 1); add(t + 0.30, 1, button, 0); }
        else if (!strcmp(act, "key")) { add(t, 2, a, 1); add(t + 0.1, 2, a, 0); }
        else if (!strcmp(act, "char")) add(t, 3, s[used + 1 + strspn(s + used + 1, " ")], 0);
        else if (!strcmp(act, "quit")) add(t, 4, 0, 0);
        else if (!strcmp(act, "close")) add(t, 5, 0, 0);
        else die("P3_INPUT: unknown action \"%s\"", act);
        s = strchr(s, ';');
        if (s) ++s;
    }
    qsort(steps, n_steps, sizeof *steps, step_cmp);
}

// Called from the game's message and sleep calls: takes window events, runs due script steps.
void input_pump(void) {
    if (!pthread_main_np()) return;        // Cocoa and the script belong to the main thread
    window_pump();
    uint64_t now = host_ticks() - t0;
    while (next_step < n_steps && steps[next_step].t <= now) {
        Step *s = &steps[next_step++];
        switch (s->kind) {
        case 0: input_mouse_move(s->a, s->b); break;
        case 1: input_mouse_button(s->a, s->b); break;
        case 2: input_key(s->a, s->b, 0); break;
        case 3: input_key(0, 1, s->a); break;
        case 4: printf("boot: P3_INPUT quit after %u import calls\n", import_calls); exit(0);
        case 5: input_request_close(); break;
        }
    }
    if (close_requested_at && host_ticks() - close_requested_at > 2000000000ull) {
        printf("boot: closed by the user (the game did not quit by itself within 2 s)\n");
        exit(0);
    }
}

#define F(name) if (!strcmp(fn, name))
int win32_input(Cpu *c, const char *fn) {
    F("GetCursorPos") { M32(arg(c, 0)) = mouse_x; M32(arg(c, 0) + 4) = mouse_y; RET(1, 1); }
    F("SetCursorPos") { mouse_x = arg(c, 0); mouse_y = arg(c, 1); RET(1, 2); }
    F("GetMessagePos") RET((uint32_t)mouse_y << 16 | (mouse_x & 0xFFFF), 0);
    F("GetKeyState") { uint8_t k = keys[arg(c, 0) & 0xFF]; RET((k & 0x80 ? 0xFFFF8000u : 0) | (k & 1), 1); }
    F("GetAsyncKeyState") RET(keys[arg(c, 0) & 0xFF] & 0x80 ? 0xFFFF8000u : 0, 1);
    F("GetKeyboardState") { memcpy(G + arg(c, 0), keys, 256); RET(1, 1); }
    F("ToAscii") {                             // (vk, scan, state, out, flags): letters, digits, space
        uint32_t vk = arg(c, 0);
        int shift = keys[0x10] & 0x80;
        int ch = (vk >= 'A' && vk <= 'Z') ? (shift ? (int)vk : (int)vk + 32) : (vk >= '0' && vk <= '9' && !shift) || vk == ' ' ? (int)vk : 0;
        if (!ch) RET(0, 5);
        M16(arg(c, 3)) = (uint16_t)ch;
        RET(1, 5);
    }
    return 0;
}
