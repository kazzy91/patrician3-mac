// registry.c - ADVAPI32 registry functions on a text file, user_dir/registry.txt.
//
// The game keeps its hardware test results in HKLM\SOFTWARE\Ascaron\Patrizier 2 (written on Windows by
// P3Setup.exe): "Adapter" and one value per mode ("1024x768x16" ...) that is "OK", "UNTESTED" or "INAVAILABLE".
// Without them the game asks the user to run the setup tool and quits, so the defaults below mark the usual modes
// as tested. The installer's own key (Patrician3) is seeded as well. Whatever the game writes is saved back.
//
// File format, one value per line:   HKLM\SOFTWARE\Ascaron\Patrizier 2\Adapter=s:DirectDraw HAL - display
// (s: string, d: dword in hex, x: binary in hex).
#include <ctype.h>
#include <stdlib.h>

#include "host.h"

typedef struct { char key[256], name[128]; uint32_t type; uint8_t *data; uint32_t size; } Value;
static Value *vals;
static unsigned n_vals, cap_vals;
static int loaded;

static const char *const defaults[] = {
    "HKLM\\SOFTWARE\\Ascaron\\Patrizier 2\\Adapter=s:DirectDraw HAL - display",
    "HKLM\\SOFTWARE\\Ascaron\\Patrizier 2\\800x600x16=s:OK",
    "HKLM\\SOFTWARE\\Ascaron\\Patrizier 2\\800x600x32=s:OK",
    "HKLM\\SOFTWARE\\Ascaron\\Patrizier 2\\1024x768x16=s:OK",
    "HKLM\\SOFTWARE\\Ascaron\\Patrizier 2\\1024x768x32=s:OK",
    "HKLM\\SOFTWARE\\Ascaron\\Patrizier 2\\1280x1024x16=s:OK",
    "HKLM\\SOFTWARE\\Ascaron\\Patrizier 2\\1280x1024x32=s:OK",
    "HKLM\\SOFTWARE\\Ascaron\\Patrician3\\InstalledTo=s:C:\\Patrician3",
    "HKLM\\SOFTWARE\\Ascaron\\Patrician3\\InstalledFrom=s:D:\\",
    "HKLM\\SOFTWARE\\Ascaron\\Patrician3\\Language=s:ENG",
    "HKLM\\SOFTWARE\\Ascaron\\Patrician3\\Version=s:1_1",
};

static Value *find(const char *key, const char *name) {
    for (unsigned i = 0; i < n_vals; ++i)
        if (!strcasecmp(vals[i].key, key) && !strcasecmp(vals[i].name, name)) return &vals[i];
    return NULL;
}
static Value *put(const char *key, const char *name) {
    Value *v = find(key, name);
    if (v) return v;
    if (n_vals == cap_vals) { cap_vals = cap_vals ? 2 * cap_vals : 64; vals = realloc(vals, cap_vals * sizeof *vals); }
    v = &vals[n_vals++];
    memset(v, 0, sizeof *v);
    snprintf(v->key, sizeof v->key, "%s", key);
    snprintf(v->name, sizeof v->name, "%s", name);
    return v;
}
static void set(Value *v, uint32_t type, const void *data, uint32_t size) {
    free(v->data);
    v->type = type; v->size = size;
    v->data = malloc(size ? size : 1);
    memcpy(v->data, data, size);
}

static void parse_line(const char *line) {
    char l[2048];
    snprintf(l, sizeof l, "%s", line);
    l[strcspn(l, "\r\n")] = 0;
    char *eq = strchr(l, '=');
    if (!eq || eq[1] == 0 || eq[2] != ':') return;
    *eq = 0;
    char *slash = strrchr(l, '\\');
    if (!slash) return;
    *slash = 0;
    Value *v = put(l, slash + 1);
    char kind = eq[1];
    const char *val = eq + 3;
    if (kind == 's') set(v, 1, val, (uint32_t)strlen(val) + 1);           // REG_SZ
    else if (kind == 'd') { uint32_t d = (uint32_t)strtoul(val, NULL, 16); set(v, 4, &d, 4); }   // REG_DWORD
    else if (kind == 'x') {                                                // REG_BINARY
        uint8_t buf[1024];
        uint32_t n = 0;
        for (const char *p = val; isxdigit((unsigned char)p[0]) && isxdigit((unsigned char)p[1]) && n < sizeof buf; p += 2) {
            char h[3] = {p[0], p[1], 0};
            buf[n++] = (uint8_t)strtoul(h, NULL, 16);
        }
        set(v, 3, buf, n);
    }
}

static void path(char *out, size_t n) { snprintf(out, n, "%s/registry.txt", user_dir); }

static void load(void) {
    if (loaded) return;
    loaded = 1;
    for (unsigned i = 0; i < sizeof defaults / sizeof *defaults; ++i) parse_line(defaults[i]);
    char p[1100];
    path(p, sizeof p);
    FILE *f = fopen(p, "r");
    if (!f) return;
    char line[2048];
    while (fgets(line, sizeof line, f)) parse_line(line);
    fclose(f);
}
static void save(void) {
    char p[1100];
    path(p, sizeof p);
    FILE *f = fopen(p, "w");
    if (!f) return;
    for (unsigned i = 0; i < n_vals; ++i) {
        Value *v = &vals[i];
        if (v->type == 1 || v->type == 2) fprintf(f, "%s\\%s=s:%.*s\n", v->key, v->name, (int)strnlen((char *)v->data, v->size), (char *)v->data);
        else if (v->type == 4 && v->size == 4) fprintf(f, "%s\\%s=d:%08X\n", v->key, v->name, *(uint32_t *)v->data);
        else {
            fprintf(f, "%s\\%s=x:", v->key, v->name);
            for (uint32_t k = 0; k < v->size; ++k) fprintf(f, "%02X", v->data[k]);
            fprintf(f, "\n");
        }
    }
    fclose(f);
}

// ---------------- key handles ----------------
#define KEY_BASE 0x3000u
static char keys[64][256];
static int keys_used[64];

static int root_name(uint32_t h, char *out, size_t n) {
    switch (h) {
    case 0x80000000: snprintf(out, n, "HKCR"); return 1;
    case 0x80000001: snprintf(out, n, "HKCU"); return 1;
    case 0x80000002: snprintf(out, n, "HKLM"); return 1;
    case 0x80000003: snprintf(out, n, "HKU"); return 1;
    }
    uint32_t i = h - KEY_BASE;
    if (i < 64 && keys_used[i]) { snprintf(out, n, "%s", keys[i]); return 1; }
    return 0;
}
static int key_exists(const char *key) {
    size_t n = strlen(key);
    for (unsigned i = 0; i < n_vals; ++i)
        if (!strncasecmp(vals[i].key, key, n) && (vals[i].key[n] == 0 || vals[i].key[n] == '\\')) return 1;
    return 0;
}
static uint32_t open_key(const char *full) {
    for (int i = 0; i < 64; ++i)
        if (!keys_used[i]) { keys_used[i] = 1; snprintf(keys[i], sizeof keys[i], "%s", full); return KEY_BASE + i; }
    die("too many open registry keys");
}
static void subkey(uint32_t root, uint32_t sub, char *out, size_t n) {
    char r[256];
    if (!root_name(root, r, sizeof r)) snprintf(r, sizeof r, "?");
    if (sub && *gptr(sub)) snprintf(out, n, "%s\\%s", r, gptr(sub)); else snprintf(out, n, "%s", r);
}

#define F(name) if (!strcmp(fn, name))
int win32_advapi(Cpu *c, const char *fn) {
    load();
    F("RegOpenKeyExA") {                       // (root, sub, opts, sam, out)
        char full[512];
        subkey(arg(c, 0), arg(c, 1), full, sizeof full);
        int ok = key_exists(full);
        trace("      RegOpenKeyExA(%s) -> %s\n", full, ok ? "open" : "not found");
        if (!ok) RET(2, 5);                    // ERROR_FILE_NOT_FOUND
        M32(arg(c, 4)) = open_key(full);
        RET(0, 5);
    }
    F("RegCreateKeyExA") {                     // (root, sub, 0, class, opts, sam, sa, out, disposition)
        char full[512];
        subkey(arg(c, 0), arg(c, 1), full, sizeof full);
        int existed = key_exists(full);
        trace("      RegCreateKeyExA(%s) -> %s\n", full, existed ? "opened" : "created");
        M32(arg(c, 7)) = open_key(full);
        if (arg(c, 8)) M32(arg(c, 8)) = existed ? 2 : 1;   // REG_OPENED_EXISTING_KEY / REG_CREATED_NEW_KEY
        RET(0, 9);
    }
    F("RegCloseKey") {
        uint32_t i = arg(c, 0) - KEY_BASE;
        if (i < 64) keys_used[i] = 0;
        RET(0, 1);
    }
    F("RegQueryValueExA") {                    // (key, name, 0, type*, data*, size*)
        char key[256];
        if (!root_name(arg(c, 0), key, sizeof key)) RET(6, 6);   // ERROR_INVALID_HANDLE
        const char *name = arg(c, 1) ? gptr(arg(c, 1)) : "";
        Value *v = find(key, name);
        trace("      RegQueryValueExA(%s\\%s) -> %s\n", key, name, v ? "found" : "not found");
        if (!v) RET(2, 6);
        if (arg(c, 3)) M32(arg(c, 3)) = v->type;
        uint32_t cap = arg(c, 5) ? M32(arg(c, 5)) : 0;
        if (arg(c, 5)) M32(arg(c, 5)) = v->size;
        if (arg(c, 4)) {
            if (cap < v->size) RET(234, 6);    // ERROR_MORE_DATA
            memcpy(G + arg(c, 4), v->data, v->size);
        }
        RET(0, 6);
    }
    F("RegSetValueExA") {                      // (key, name, 0, type, data, size)
        char key[256];
        if (!root_name(arg(c, 0), key, sizeof key)) RET(6, 6);
        Value *v = put(key, arg(c, 1) ? gptr(arg(c, 1)) : "");
        set(v, arg(c, 3), G + arg(c, 4), arg(c, 5));
        save();
        RET(0, 6);
    }
    return 0;
}
