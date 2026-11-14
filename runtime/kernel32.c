// kernel32.c - the KERNEL32 subset Patrician3.exe uses (besides heap.c and files.c).
//
// Single-threaded for now: critical sections are no-ops, CreateThread records the thread without running it, and
// waits return at once. Threads (the game uses a few) come with the GIL model of the NFS port's threads.c.
#include <ctype.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#include "host.h"
#include "module.h"

int files_close(uint32_t h);

// ---------------- PE resources ----------------
// Walk the image's resource directory. Names/types are either integer IDs (MAKEINTRESOURCE: < 0x10000) or guest
// strings ("#123" is also an ID). Returns the guest address of the IMAGE_RESOURCE_DATA_ENTRY, or 0.
static uint32_t rsrc_base(void) {
    uint32_t off = M32(IMAGE_BASE + 0x3C);
    uint32_t rva = M32(IMAGE_BASE + off + 24 + 96 + 2 * 8);
    return rva ? IMAGE_BASE + rva : 0;
}
static int name_matches(uint32_t root, uint32_t entry_name, uint32_t want) {
    if (want < 0x10000) return !(entry_name & 0x80000000) && entry_name == want;
    const char *s = gptr(want);
    if (s[0] == '#') return !(entry_name & 0x80000000) && entry_name == (uint32_t)atoi(s + 1);
    if (!(entry_name & 0x80000000)) return 0;
    uint32_t p = root + (entry_name & 0x7FFFFFFF);
    uint16_t n = M16(p);
    if (strlen(s) != n) return 0;
    for (unsigned i = 0; i < n; ++i)
        if (toupper((unsigned char)s[i]) != toupper(M16(p + 2 + 2 * i) & 0xFF)) return 0;
    return 1;
}
static uint32_t dir_find(uint32_t root, uint32_t dir, uint32_t want) {
    unsigned n = M16(dir + 12) + M16(dir + 14);
    for (unsigned i = 0; i < n; ++i) {
        uint32_t e = dir + 16 + 8 * i;
        if (want == 0xFFFFFFFF || name_matches(root, M32(e), want)) return M32(e + 4);
    }
    return 0xFFFFFFFF;
}
uint32_t find_resource(uint32_t name, uint32_t type) {
    uint32_t root = rsrc_base();
    if (!root) return 0;
    uint32_t t = dir_find(root, root, type);
    if (t == 0xFFFFFFFF || !(t & 0x80000000)) return 0;
    uint32_t nm = dir_find(root, root + (t & 0x7FFFFFFF), name);
    if (nm == 0xFFFFFFFF || !(nm & 0x80000000)) return 0;
    uint32_t lang = dir_find(root, root + (nm & 0x7FFFFFFF), 0xFFFFFFFF);   // first language
    if (lang == 0xFFFFFFFF || (lang & 0x80000000)) return 0;
    return root + lang;
}
uint32_t resource_data(uint32_t entry) { return entry ? IMAGE_BASE + M32(entry) : 0; }
uint32_t resource_size(uint32_t entry) { return entry ? M32(entry + 4) : 0; }

// ---------------- INI files ----------------
static int ini_get(const char *file, const char *sec, const char *key, char *out, size_t n) {
    char host[2048];
    path_resolve(file, host, sizeof host, 0);
    FILE *f = fopen(host, "r");
    if (!f) return 0;
    char line[1024], cur[256] = "";
    int found = 0;
    while (fgets(line, sizeof line, f)) {
        char *s = line;
        while (*s == ' ' || *s == '\t') ++s;
        s[strcspn(s, "\r\n")] = 0;
        if (*s == '[') { char *e = strchr(s, ']'); if (e) *e = 0; snprintf(cur, sizeof cur, "%s", s + 1); continue; }
        char *eq = strchr(s, '=');
        if (!eq || strcasecmp(cur, sec)) continue;
        *eq = 0;
        char *k = s, *ke = eq;
        while (ke > k && (ke[-1] == ' ' || ke[-1] == '\t')) *--ke = 0;
        if (strcasecmp(k, key)) continue;
        char *v = eq + 1;
        while (*v == ' ' || *v == '\t') ++v;
        snprintf(out, n, "%s", v);
        found = 1;
        break;
    }
    fclose(f);
    return found;
}
static int ini_set(const char *file, const char *sec, const char *key, const char *val) {
    char src[2048], dst[2048];
    path_resolve(file, src, sizeof src, 0);
    path_resolve(file, dst, sizeof dst, 1);
    FILE *in = fopen(src, "r");
    char *buf = NULL;
    size_t len = 0;
    FILE *mem = open_memstream(&buf, &len);
    char line[1024], cur[256] = "";
    int done = 0, in_sec = 0;
    while (in && fgets(line, sizeof line, in)) {
        char tmp[1024];
        snprintf(tmp, sizeof tmp, "%s", line);
        char *s = tmp;
        while (*s == ' ' || *s == '\t') ++s;
        if (*s == '[') {
            if (in_sec && !done && key && val) { fprintf(mem, "%s=%s\n", key, val); done = 1; }
            char *e = strchr(s, ']'); if (e) *e = 0;
            snprintf(cur, sizeof cur, "%s", s + 1);
            in_sec = !strcasecmp(cur, sec);
            if (in_sec && !key) continue;      // deleting the section
            fputs(line, mem);
            continue;
        }
        if (in_sec) {
            if (!key) continue;
            char *eq = strchr(s, '=');
            if (eq) {
                *eq = 0;
                char *ke = eq;
                while (ke > s && (ke[-1] == ' ' || ke[-1] == '\t')) *--ke = 0;
                if (!strcasecmp(s, key)) { if (val && !done) { fprintf(mem, "%s=%s\n", key, val); done = 1; } continue; }
            }
        }
        fputs(line, mem);
    }
    if (in) fclose(in);
    if (!done && key && val) {
        if (!in_sec) fprintf(mem, "[%s]\n", sec);
        fprintf(mem, "%s=%s\n", key, val);
    }
    fclose(mem);
    FILE *out = fopen(dst, "w");
    if (out) { fwrite(buf, 1, len, out); fclose(out); }
    free(buf);
    return out != NULL;
}

// ---------------- small handle tables ----------------
#define OBJ_BASE 0x2000u
enum { OBJ_FREE, OBJ_EVENT, OBJ_THREAD };
static struct { int kind; uint32_t state, start, param; } objs[512];
static uint32_t obj_new(int kind) {
    for (int i = 0; i < 512; ++i) if (objs[i].kind == OBJ_FREE) { objs[i].kind = kind; objs[i].state = 0; return OBJ_BASE + i; }
    die("too many kernel objects");
}

static uint32_t tls[64];
static uint64_t tls_used;
static struct { uint32_t used; char name[256]; uint32_t refs; } atoms[256];

static uint16_t cp1252_to_unicode(uint8_t b) { return b; }   // Latin-1 (good enough for the game's German/English text)

#define F(name) if (!strcmp(fn, name))
int win32_kernel32(Cpu *c, const char *fn) {
    // ---- version, process, modules ----
    F("GetVersion") RET(0x0A280105, 0);                         // 5.1 build 2600 (XP), NT
    F("GetVersionExA") {
        uint32_t p = arg(c, 0);
        M32(p + 4) = 5; M32(p + 8) = 1; M32(p + 12) = 2600; M32(p + 16) = 2;
        strcpy((char *)G + p + 20, "Service Pack 2");
        RET(1, 1);
    }
    F("GetProcessVersion") RET(0x00040000, 1);
    F("GetModuleHandleA") {
        if (arg(c, 0) == 0) RET(IMAGE_BASE, 1);
        const char *n = gptr(arg(c, 0));
        const ModuleTables *m = module_by_name(n);
        trace("      GetModuleHandleA(\"%s\") -> %s\n", n, m ? m->file : "system or absent");
        if (m) RET(m->base, 1);
        RET(strcasestr(n, "kernel32") || strcasestr(n, "user32") || strcasestr(n, "ntdll") ? 0x7C800000 : 0, 1);
    }
    F("GetModuleFileNameA") {
        const ModuleTables *m = module_by_base(arg(c, 0) ? arg(c, 0) : IMAGE_BASE);
        char s[300];
        path_guest_dir(s, sizeof s);
        strlcat(s, "\\", sizeof s);
        strlcat(s, m ? m->file : "Patrician3.exe", sizeof s);
        snprintf((char *)G + arg(c, 1), arg(c, 2), "%s", s);
        RET(strlen(s), 3);
    }
    F("LoadLibraryA") {
        const ModuleTables *m = module_by_name(gptr(arg(c, 0)));
        fprintf(stderr, "boot: LoadLibraryA(\"%s\") -> %s\n", gptr(arg(c, 0)), m ? "translated module" : "not available yet");
        if (m) RET(m->base, 1);
        last_error = 126;
        RET(0, 1);
    }
    F("FreeLibrary") RET(1, 1);
    F("GetProcAddress") {
        uint32_t p = arg(c, 1);
        const ModuleTables *m = module_by_base(arg(c, 0));
        char name[256];
        if (p < 0x10000) snprintf(name, sizeof name, "#%u", p); else snprintf(name, sizeof name, "%s", gptr(p));
        uint32_t a = m ? module_export(m, name) : 0;
        trace("      GetProcAddress(%08X, %s) -> %08X\n", arg(c, 0), name, a);
        if (!a) last_error = 127;
        RET(a, 2);
    }
    F("GetCommandLineA") {
        static uint32_t cl;
        if (!cl) cl = gstr("\"C:\\Patrician3\\Patrician3.exe\"");
        RET(cl, 0);
    }
    F("GetStartupInfoA") {
        uint32_t p = arg(c, 0);
        memset(G + p, 0, 68);
        M32(p) = 68;
        RET(0, 1);
    }
    F("GetStdHandle") RET(0x100 + (arg(c, 0) & 0xFF), 1);
    F("SetStdHandle") RET(1, 2);
    F("SetHandleCount") RET(arg(c, 0), 1);
    F("GetEnvironmentStrings") { uint32_t a = heap_alloc(4, 1); RET(a, 0); }
    F("GetEnvironmentStringsW") { uint32_t a = heap_alloc(4, 1); RET(a, 0); }
    F("FreeEnvironmentStringsA") RET(1, 1);
    F("FreeEnvironmentStringsW") RET(1, 1);
    F("GetEnvironmentVariableA") { last_error = 203; RET(0, 3); }
    F("SetEnvironmentVariableA") RET(1, 2);
    F("GetCurrentProcess") RET(0xFFFFFFFF, 0);
    F("GetCurrentThread") RET(0xFFFFFFFE, 0);
    F("GetCurrentThreadId") RET(1, 0);
    F("ExitProcess") { fflush(stderr); printf("boot: ExitProcess(%u) after %u import calls\n", arg(c, 0), import_calls); exit((int)arg(c, 0)); }
    F("TerminateProcess") { printf("boot: TerminateProcess(%u)\n", arg(c, 1)); exit((int)arg(c, 1)); }
    F("CreateProcessA") { last_error = 2; RET(0, 10); }
    F("SetErrorMode") RET(0, 1);
    F("SetUnhandledExceptionFilter") RET(0, 1);
    F("UnhandledExceptionFilter") RET(1, 1);                     // EXCEPTION_EXECUTE_HANDLER
    F("GetLastError") RET(last_error, 0);
    F("SetLastError") { last_error = arg(c, 0); RET(0, 1); }
    F("IsBadReadPtr") RET(arg(c, 0) == 0, 2);
    F("IsBadWritePtr") RET(arg(c, 0) == 0, 2);
    F("IsBadCodePtr") RET(arg(c, 0) == 0, 1);
    F("FormatMessageA") {
        char s[64];
        snprintf(s, sizeof s, "Error %u\r\n", arg(c, 2));
        if (arg(c, 0) & 0x100) { M32(arg(c, 4)) = gstr(s); RET(strlen(s), 7); }   // FORMAT_MESSAGE_ALLOCATE_BUFFER
        snprintf((char *)G + arg(c, 4), arg(c, 5), "%s", s);
        RET(strlen(s), 7);
    }
    F("RaiseException") die("RaiseException(code %08X, flags %X, %u args) from %08X: C++ throw or SEH raise not supported yet",
                            arg(c, 0), arg(c, 1), arg(c, 2), M32(c->esp));
    F("RtlUnwind") die("RtlUnwind from %08X: SEH unwinding not supported yet", M32(c->esp));
    F("DuplicateHandle") { if (arg(c, 3)) M32(arg(c, 3)) = arg(c, 1); RET(1, 7); }

    // ---- code pages and strings ----
    F("GetACP") RET(1252, 0);
    F("GetOEMCP") RET(850, 0);
    F("GetCPInfo") {
        uint32_t p = arg(c, 1);
        memset(G + p, 0, 20);
        M32(p) = 1; M8(p + 4) = '?';
        RET(1, 2);
    }
    F("MultiByteToWideChar") {
        uint32_t src = arg(c, 2), dst = arg(c, 4);
        int n = (int32_t)arg(c, 3), cap = (int32_t)arg(c, 5);
        if (n < 0) n = (int)strlen(gptr(src)) + 1;
        if (!cap) RET(n, 6);
        int k = n < cap ? n : cap;
        for (int i = 0; i < k; ++i) M16(dst + 2 * i) = cp1252_to_unicode(M8(src + i));
        RET(k, 6);
    }
    F("WideCharToMultiByte") {
        uint32_t src = arg(c, 2), dst = arg(c, 4);
        int n = (int32_t)arg(c, 3), cap = (int32_t)arg(c, 5);
        if (n < 0) { n = 0; while (M16(src + 2 * n)) ++n; ++n; }
        if (!cap) RET(n, 8);
        int k = n < cap ? n : cap;
        for (int i = 0; i < k; ++i) { uint16_t w = M16(src + 2 * i); M8(dst + i) = w < 256 ? (uint8_t)w : '?'; }
        RET(k, 8);
    }
    F("GetStringTypeA") {      // (lcid, type, src, n, out)
        uint32_t src = arg(c, 2), out = arg(c, 4);
        int n = (int32_t)arg(c, 3);
        if (n < 0) n = (int)strlen(gptr(src)) + 1;
        for (int i = 0; i < n; ++i) {
            uint8_t b = M8(src + i);
            uint16_t t = 0;
            if (isupper(b)) t |= 0x001; if (islower(b)) t |= 0x002; if (isdigit(b)) t |= 0x004; if (isspace(b)) t |= 0x008;
            if (ispunct(b)) t |= 0x010; if (iscntrl(b)) t |= 0x020; if (b == ' ' || b == '\t') t |= 0x040;
            if (isxdigit(b)) t |= 0x080; if (isalpha(b) || b >= 0xC0) t |= 0x100 | (b >= 0xC0 ? (b < 0xE0 ? 1 : 2) : 0);
            M16(out + 2 * i) = t;
        }
        RET(1, 5);
    }
    F("GetStringTypeW") {      // (type, src, n, out)
        uint32_t src = arg(c, 1), out = arg(c, 3);
        int n = (int32_t)arg(c, 2);
        if (n < 0) { n = 0; while (M16(src + 2 * n)) ++n; ++n; }
        for (int i = 0; i < n; ++i) {
            uint16_t w = M16(src + 2 * i), t = 0;
            int b = w < 256 ? w : 'a';
            if (isupper(b)) t |= 0x001; if (islower(b)) t |= 0x002; if (isdigit(b)) t |= 0x004; if (isspace(b)) t |= 0x008;
            if (ispunct(b)) t |= 0x010; if (iscntrl(b)) t |= 0x020; if (b == ' ' || b == '\t') t |= 0x040;
            if (isxdigit(b)) t |= 0x080; if (isalpha(b)) t |= 0x100;
            M16(out + 2 * i) = t;
        }
        RET(1, 4);
    }
    F("LCMapStringA") {        // (lcid, flags, src, n, dst, cap)
        uint32_t flags = arg(c, 1), src = arg(c, 2), dst = arg(c, 4);
        int n = (int32_t)arg(c, 3), cap = (int32_t)arg(c, 5);
        if (n < 0) n = (int)strlen(gptr(src)) + 1;
        if (!cap) RET(n, 6);
        for (int i = 0; i < n && i < cap; ++i) {
            uint8_t b = M8(src + i);
            M8(dst + i) = flags & 0x100 ? tolower(b) : flags & 0x200 ? toupper(b) : b;   // LCMAP_LOWERCASE / UPPERCASE
        }
        RET(n < cap ? n : cap, 6);
    }
    F("LCMapStringW") {
        uint32_t flags = arg(c, 1), src = arg(c, 2), dst = arg(c, 4);
        int n = (int32_t)arg(c, 3), cap = (int32_t)arg(c, 5);
        if (n < 0) { n = 0; while (M16(src + 2 * n)) ++n; ++n; }
        if (!cap) RET(n, 6);
        for (int i = 0; i < n && i < cap; ++i) {
            uint16_t w = M16(src + 2 * i);
            M16(dst + 2 * i) = w < 256 ? (flags & 0x100 ? tolower(w) : flags & 0x200 ? toupper(w) : w) : w;
        }
        RET(n < cap ? n : cap, 6);
    }
    F("CompareStringA") {      // (lcid, flags, s1, n1, s2, n2) -> 1 <, 2 =, 3 >
        const char *a = gptr(arg(c, 2)), *b = gptr(arg(c, 4));
        int na = (int32_t)arg(c, 3), nb = (int32_t)arg(c, 5);
        if (na < 0) na = (int)strlen(a);
        if (nb < 0) nb = (int)strlen(b);
        int m = na < nb ? na : nb;
        int r = (arg(c, 1) & 1) ? strncasecmp(a, b, m) : strncmp(a, b, m);
        if (!r) r = na - nb;
        RET(r < 0 ? 1 : r > 0 ? 3 : 2, 6);
    }
    F("CompareStringW") {
        uint32_t a = arg(c, 2), b = arg(c, 4);
        int na = (int32_t)arg(c, 3), nb = (int32_t)arg(c, 5);
        if (na < 0) { na = 0; while (M16(a + 2 * na)) ++na; }
        if (nb < 0) { nb = 0; while (M16(b + 2 * nb)) ++nb; }
        int r = 0;
        for (int i = 0; !r && i < na && i < nb; ++i) {
            int x = M16(a + 2 * i), y = M16(b + 2 * i);
            if (arg(c, 1) & 1) { x = x < 256 ? tolower(x) : x; y = y < 256 ? tolower(y) : y; }
            r = x - y;
        }
        if (!r) r = na - nb;
        RET(r < 0 ? 1 : r > 0 ? 3 : 2, 6);
    }
    F("lstrlenA") RET(arg(c, 0) ? strlen(gptr(arg(c, 0))) : 0, 1);
    F("lstrcpyA") { strcpy((char *)G + arg(c, 0), gptr(arg(c, 1))); RET(arg(c, 0), 2); }
    F("lstrcpynA") {
        int n = (int32_t)arg(c, 2);
        if (n > 0) { strncpy((char *)G + arg(c, 0), gptr(arg(c, 1)), n - 1); M8(arg(c, 0) + n - 1) = 0; }
        RET(arg(c, 0), 3);
    }
    F("lstrcatA") { strcat((char *)G + arg(c, 0), gptr(arg(c, 1))); RET(arg(c, 0), 2); }
    F("lstrcmpA") RET(strcmp(gptr(arg(c, 0)), gptr(arg(c, 1))), 2);
    F("lstrcmpiA") RET(strcasecmp(gptr(arg(c, 0)), gptr(arg(c, 1))), 2);
    F("MulDiv") {
        int64_t a = (int32_t)arg(c, 0), b = (int32_t)arg(c, 1), d = (int32_t)arg(c, 2);
        if (!d) RET(-1, 3);
        int64_t p = a * b;
        RET((int32_t)((p + (p >= 0 ? d / 2 : -d / 2)) / d), 3);
    }

    // ---- TLS, synchronisation, threads (single-threaded for now) ----
    F("TlsAlloc") {
        for (int i = 0; i < 64; ++i) if (!(tls_used >> i & 1)) { tls_used |= 1ull << i; tls[i] = 0; RET(i, 0); }
        RET(0xFFFFFFFF, 0);
    }
    F("TlsFree") { if (arg(c, 0) < 64) tls_used &= ~(1ull << arg(c, 0)); RET(1, 1); }
    F("TlsGetValue") { last_error = 0; RET(arg(c, 0) < 64 ? tls[arg(c, 0)] : 0, 1); }
    F("TlsSetValue") { if (arg(c, 0) < 64) tls[arg(c, 0)] = arg(c, 1); RET(1, 2); }
    F("InitializeCriticalSection") { memset(G + arg(c, 0), 0, 24); RET(0, 1); }
    F("DeleteCriticalSection") RET(0, 1);
    F("EnterCriticalSection") RET(0, 1);
    F("LeaveCriticalSection") RET(0, 1);
    F("InterlockedIncrement") { M32(arg(c, 0)) += 1; RET(M32(arg(c, 0)), 1); }
    F("InterlockedDecrement") { M32(arg(c, 0)) -= 1; RET(M32(arg(c, 0)), 1); }
    F("InterlockedExchange") { uint32_t o = M32(arg(c, 0)); M32(arg(c, 0)) = arg(c, 1); RET(o, 2); }
    F("CreateEventA") { uint32_t h = obj_new(OBJ_EVENT); objs[h - OBJ_BASE].state = arg(c, 2); RET(h, 4); }
    F("SetEvent") { uint32_t i = arg(c, 0) - OBJ_BASE; if (i < 512) objs[i].state = 1; RET(1, 1); }
    F("WaitForSingleObject") {
        uint32_t i = arg(c, 0) - OBJ_BASE;
        trace("      WaitForSingleObject(%08X, %u) -> returns at once (single-threaded)\n", arg(c, 0), arg(c, 1));
        if (i < 512 && objs[i].kind == OBJ_EVENT && !objs[i].state && arg(c, 1) != 0xFFFFFFFF) RET(0x102, 2);   // WAIT_TIMEOUT
        RET(0, 2);
    }
    F("CreateThread") {
        uint32_t h = obj_new(OBJ_THREAD);
        objs[h - OBJ_BASE].start = arg(c, 2);
        objs[h - OBJ_BASE].param = arg(c, 3);
        fprintf(stderr, "boot: CreateThread(start %08X, param %08X, flags %X) recorded, not run (threads not implemented)\n",
                arg(c, 2), arg(c, 3), arg(c, 4));
        if (arg(c, 5)) M32(arg(c, 5)) = 2 + (h - OBJ_BASE);
        RET(h, 6);
    }
    F("ResumeThread") RET(1, 1);
    F("SuspendThread") RET(0, 1);
    F("SetThreadPriority") RET(1, 2);
    F("ExitThread") die("ExitThread(%u) on the main thread", arg(c, 0));
    F("CloseHandle") {
        uint32_t h = arg(c, 0);
        if (files_close(h)) RET(1, 1);
        if (h - OBJ_BASE < 512) objs[h - OBJ_BASE].kind = OBJ_FREE;
        RET(1, 1);
    }
    F("Sleep") { if (arg(c, 0)) usleep(arg(c, 0) * 1000u); RET(0, 1); }

    // ---- time ----
    F("GetLocalTime") {
        time_t t = time(NULL);
        struct tm tm;
        localtime_r(&t, &tm);
        uint32_t p = arg(c, 0);
        M16(p) = tm.tm_year + 1900; M16(p + 2) = tm.tm_mon + 1; M16(p + 4) = tm.tm_wday; M16(p + 6) = tm.tm_mday;
        M16(p + 8) = tm.tm_hour; M16(p + 10) = tm.tm_min; M16(p + 12) = tm.tm_sec; M16(p + 14) = 0;
        RET(0, 1);
    }
    F("GetTimeZoneInformation") { memset(G + arg(c, 0), 0, 172); RET(0, 1); }   // TIME_ZONE_ID_UNKNOWN, UTC
    F("FileTimeToLocalFileTime") { M64(arg(c, 1)) = M64(arg(c, 0)); RET(1, 2); }
    F("FileTimeToSystemTime") {
        time_t t = (time_t)((M64(arg(c, 0)) - 116444736000000000ull) / 10000000ull);
        struct tm tm;
        gmtime_r(&t, &tm);
        uint32_t p = arg(c, 1);
        M16(p) = tm.tm_year + 1900; M16(p + 2) = tm.tm_mon + 1; M16(p + 4) = tm.tm_wday; M16(p + 6) = tm.tm_mday;
        M16(p + 8) = tm.tm_hour; M16(p + 10) = tm.tm_min; M16(p + 12) = tm.tm_sec; M16(p + 14) = 0;
        RET(1, 2);
    }

    // ---- INI files ----
    F("GetPrivateProfileStringA") {   // (sec, key, default, out, size, file)
        char v[1024];
        const char *def = arg(c, 2) ? gptr(arg(c, 2)) : "";
        if (!arg(c, 0) || !arg(c, 1)) die("GetPrivateProfileStringA section/key enumeration not supported yet");
        if (!ini_get(gptr(arg(c, 5)), gptr(arg(c, 0)), gptr(arg(c, 1)), v, sizeof v)) snprintf(v, sizeof v, "%s", def);
        snprintf((char *)G + arg(c, 3), arg(c, 4), "%s", v);
        trace("      GetPrivateProfileStringA([%s] %s, %s) = \"%s\"\n", gptr(arg(c, 0)), gptr(arg(c, 1)), gptr(arg(c, 5)), v);
        RET(strlen((char *)G + arg(c, 3)), 6);
    }
    F("GetPrivateProfileIntA") {      // (sec, key, default, file)
        char v[64];
        if (!ini_get(gptr(arg(c, 3)), gptr(arg(c, 0)), gptr(arg(c, 1)), v, sizeof v)) RET(arg(c, 2), 4);
        RET((uint32_t)atoi(v), 4);
    }
    F("WritePrivateProfileStringA") {  // (sec, key, value, file)
        RET(ini_set(gptr(arg(c, 3)), gptr(arg(c, 0)), arg(c, 1) ? gptr(arg(c, 1)) : NULL, arg(c, 2) ? gptr(arg(c, 2)) : NULL), 4);
    }

    // ---- resources ----
    F("FindResourceA") RET(find_resource(arg(c, 1), arg(c, 2)), 3);
    F("LoadResource") RET(resource_data(arg(c, 1)), 2);
    F("LockResource") RET(arg(c, 0), 1);

    // ---- atoms ----
    F("GlobalAddAtomA") {
        const char *s = gptr(arg(c, 0));
        if (arg(c, 0) < 0x10000) RET(arg(c, 0), 1);
        for (int i = 0; i < 256; ++i) if (atoms[i].used && !strcasecmp(atoms[i].name, s)) { ++atoms[i].refs; RET(0xC000 + i, 1); }
        for (int i = 0; i < 256; ++i) if (!atoms[i].used) {
            atoms[i].used = 1; atoms[i].refs = 1;
            snprintf(atoms[i].name, sizeof atoms[i].name, "%s", s);
            RET(0xC000 + i, 1);
        }
        RET(0, 1);
    }
    F("GlobalFindAtomA") {
        const char *s = gptr(arg(c, 0));
        for (int i = 0; i < 256; ++i) if (atoms[i].used && !strcasecmp(atoms[i].name, s)) RET(0xC000 + i, 1);
        RET(0, 1);
    }
    F("GlobalDeleteAtom") {
        uint32_t i = arg(c, 0) - 0xC000;
        if (i < 256 && atoms[i].used && !--atoms[i].refs) atoms[i].used = 0;
        RET(0, 1);
    }
    F("GlobalGetAtomNameA") {
        uint32_t i = arg(c, 0) - 0xC000;
        if (i >= 256 || !atoms[i].used) RET(0, 3);
        snprintf((char *)G + arg(c, 1), arg(c, 2), "%s", atoms[i].name);
        RET(strlen((char *)G + arg(c, 1)), 3);
    }
    return 0;
}
