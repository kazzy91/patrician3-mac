// files.c - Windows file API on host files.
//
// The game believes it runs from C:\Patrician3. Paths under it (or relative to the current directory) resolve
// case-insensitively: reads look in user_dir first (anything the game wrote), then in the game folder; writes always
// go to user_dir, so the user's installation is never modified.
#include <dirent.h>
#include <sys/mman.h>
#include <errno.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "host.h"

#define GUEST_ROOT "C:\\Patrician3"
static char cwd[260] = GUEST_ROOT;

void path_guest_dir(char *out, size_t n) { snprintf(out, n, "%s", GUEST_ROOT); }

// Normalise a Windows path to a relative path under the guest root with '/' separators; 0 if outside it.
static int guest_rel(const char *win, char *rel, size_t n) {
    char full[1024];
    if ((win[0] && win[1] == ':') || win[0] == '\\' || win[0] == '/')
        snprintf(full, sizeof full, "%s", win[0] == '\\' || win[0] == '/' ? win + 0 : win);
    else
        snprintf(full, sizeof full, "%s\\%s", cwd, win);
    for (char *p = full; *p; ++p) if (*p == '/') *p = '\\';
    size_t rl = strlen(GUEST_ROOT);
    const char *r;
    if (!strncasecmp(full, GUEST_ROOT, rl) && (full[rl] == '\\' || !full[rl])) r = full + rl;
    else if (full[0] == '\\') r = full;                       // "\x" relative to the drive: treat as the root
    else return 0;
    // collapse ".\" and "..\"
    char parts[64][260];
    int np = 0;
    char tmp[1024];
    snprintf(tmp, sizeof tmp, "%s", r);
    for (char *t = strtok(tmp, "\\"); t; t = strtok(NULL, "\\")) {
        if (!strcmp(t, ".") || !*t) continue;
        if (!strcmp(t, "..")) { if (np) --np; continue; }
        if (np < 64) snprintf(parts[np++], 260, "%s", t);
    }
    rel[0] = 0;
    for (int i = 0; i < np; ++i) {
        strlcat(rel, i ? "/" : "", n);
        strlcat(rel, parts[i], n);
    }
    return 1;
}

// Case-insensitive lookup of rel under dir. out gets the host path (best effort for missing components).
static int lookup(const char *dir, const char *rel, char *out, size_t n) {
    snprintf(out, n, "%s", dir);
    char tmp[1024];
    snprintf(tmp, sizeof tmp, "%s", rel);
    int found = 1;
    for (char *t = strtok(tmp, "/"); t; t = strtok(NULL, "/")) {
        char match[260] = "";
        DIR *d = found ? opendir(out) : NULL;
        if (d) {
            struct dirent *e;
            while ((e = readdir(d)))
                if (!strcasecmp(e->d_name, t)) { snprintf(match, sizeof match, "%s", e->d_name); break; }
            closedir(d);
        }
        if (!match[0]) { found = 0; snprintf(match, sizeof match, "%s", t); }
        strlcat(out, "/", n);
        strlcat(out, match, n);
    }
    return found;
}

int path_resolve(const char *win, char *out, size_t n, int for_write) {
    char rel[1024];
    if (!guest_rel(win, rel, sizeof rel)) { snprintf(out, n, "/nonexistent/%s", win); return 0; }
    if (lookup(user_dir, rel, out, n)) return 1;
    if (for_write) {
        // create the parent directories in user_dir
        char p[1024];
        snprintf(p, sizeof p, "%s", out);
        for (char *s = p + strlen(user_dir) + 1; (s = strchr(s, '/')); ++s) { *s = 0; mkdir(p, 0755); *s = '/'; }
        return 1;
    }
    return lookup(game_dir, rel, out, n);
}

// ---------------- handles ----------------
#define FILE_BASE 0x1000u
#define NFILES 256
static struct { int fd; int used; } files[NFILES];
#define FIND_BASE 0x1800u
static struct { DIR *d; char dir[1024]; char pattern[260]; int used; } finds[16];

static uint32_t file_handle(int fd) {
    for (int i = 0; i < NFILES; ++i)
        if (!files[i].used) { files[i].used = 1; files[i].fd = fd; return FILE_BASE + i; }
    die("too many open files");
}
static int fd_of(uint32_t h) {
    if (h >= FILE_BASE && h < FILE_BASE + NFILES && files[h - FILE_BASE].used) return files[h - FILE_BASE].fd;
    return -1;
}
// ---------------- file mappings ----------------
// Views are host mmaps of the file placed into guest memory at [MAP_BASE, MAP_END) (MAP_FIXED over the 4 GB
// reservation), so a 240 MB archive costs no copy. Private (copy-on-write): the game only maps archives read-only.
#define MAP_BASE 0x61000000u
#define MAP_END  0x7E000000u
#define MAPPING_BASE 0x1A00u
static struct { int used, fd; uint64_t size; } mappings[32];
static struct { uint32_t addr, size; } views[64];
static uint32_t map_next = MAP_BASE;

static uint32_t map_view(int fd, uint64_t off, uint32_t size) {
    uint32_t page = (uint32_t)getpagesize();
    uint64_t aligned = off & ~(uint64_t)(page - 1);
    uint32_t lead = (uint32_t)(off - aligned), len = ((size + lead) + page - 1) & ~(page - 1);
    uint32_t a = (map_next + 0xFFFF) & ~0xFFFFu;
    if ((uint64_t)a + len > MAP_END) die("file mapping area full (%u MB mapped)", (map_next - MAP_BASE) >> 20);
    void *p = mmap(G + a, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_FIXED, fd, (off_t)aligned);
    if (p == MAP_FAILED) die("mmap of a file view failed: %s", strerror(errno));
    map_next = a + len;
    for (int i = 0; i < 64; ++i) if (!views[i].addr) { views[i].addr = a + lead; views[i].size = len; break; }
    return a + lead;
}

int files_close(uint32_t h) {
    if (h >= MAPPING_BASE && h < MAPPING_BASE + 32 && mappings[h - MAPPING_BASE].used) {
        mappings[h - MAPPING_BASE].used = 0;
        return 1;
    }
    int fd = fd_of(h);
    if (fd < 0) return 0;
    close(fd);
    files[h - FILE_BASE].used = 0;
    return 1;
}

static void fill_find_data(uint32_t p, const char *dir, const char *name) {
    // WIN32_FIND_DATAA: attrs, 3 FILETIMEs, size hi/lo, 2 reserved, cFileName[260], cAlternateFileName[14]
    char full[2048];
    snprintf(full, sizeof full, "%s/%s", dir, name);
    struct stat st;
    stat(full, &st);
    memset(G + p, 0, 320);
    M32(p) = S_ISDIR(st.st_mode) ? 0x10 : 0x80;
    uint64_t ft = (uint64_t)st.st_mtime * 10000000ull + 116444736000000000ull;
    M64(p + 4) = ft; M64(p + 12) = ft; M64(p + 20) = ft;
    M32(p + 28) = 0; M32(p + 32) = (uint32_t)st.st_size;
    snprintf((char *)G + p + 44, 260, "%s", name);
}

static int find_next(int i, uint32_t data) {
    struct dirent *e;
    while ((e = readdir(finds[i].d))) {
        if (!strcmp(e->d_name, ".") && strcmp(finds[i].pattern, "*") && strcmp(finds[i].pattern, "*.*")) continue;
        if (fnmatch(finds[i].pattern, e->d_name, FNM_CASEFOLD) == 0 ||
            (!strcmp(finds[i].pattern, "*.*"))) {
            fill_find_data(data, finds[i].dir, e->d_name);
            return 1;
        }
    }
    return 0;
}

#define F(name) if (!strcmp(fn, name))
int win32_files(Cpu *c, const char *fn) {
    F("CreateFileA") {
        const char *win = gptr(arg(c, 0));
        uint32_t acc = arg(c, 1), disp = arg(c, 4);
        int wr = (acc & 0x40000000) != 0;                 // GENERIC_WRITE
        char host[2048];
        path_resolve(win, host, sizeof host, wr);
        int fl = wr ? ((acc & 0x80000000) ? O_RDWR : O_WRONLY) : O_RDONLY;
        switch (disp) {
        case 1: fl |= O_CREAT | O_EXCL; break;             // CREATE_NEW
        case 2: fl |= O_CREAT | O_TRUNC; break;            // CREATE_ALWAYS
        case 4: fl |= O_CREAT; break;                      // OPEN_ALWAYS
        case 5: fl |= O_TRUNC; break;                      // TRUNCATE_EXISTING
        }
        if (wr && !(fl & O_TRUNC) && !(fl & O_EXCL)) {
            // a write to a file that exists only in the game folder: copy it to user_dir first
            if (access(host, F_OK) != 0) {
                char src[2048];
                char rel[1024];
                if (guest_rel(win, rel, sizeof rel) && lookup(game_dir, rel, src, sizeof src)) {
                    int in = open(src, O_RDONLY), out = open(host, O_WRONLY | O_CREAT, 0644);
                    char buf[65536];
                    ssize_t k;
                    while (in >= 0 && out >= 0 && (k = read(in, buf, sizeof buf)) > 0) write(out, buf, k);
                    if (in >= 0) close(in);
                    if (out >= 0) close(out);
                }
            }
        }
        int fd = open(host, fl, 0644);
        trace("      CreateFileA(\"%s\") -> %s%s\n", win, host, fd < 0 ? " (failed)" : "");
        if (fd < 0) { last_error = errno == ENOENT ? 2 : errno == EEXIST ? 80 : 5; RET(0xFFFFFFFF, 7); }
        last_error = 0;
        RET(file_handle(fd), 7);
    }
    F("ReadFile") {
        int fd = fd_of(arg(c, 0));
        if (fd < 0) { last_error = 6; RET(0, 5); }
        ssize_t k = read(fd, G + arg(c, 1), arg(c, 2));
        if (arg(c, 3)) M32(arg(c, 3)) = k > 0 ? (uint32_t)k : 0;
        RET(k >= 0, 5);
    }
    F("WriteFile") {
        uint32_t h = arg(c, 0);
        ssize_t k;
        if (h >= 0x100 && h < 0x110) k = write(h == 0x10C ? 2 : 1, G + arg(c, 1), arg(c, 2));   // std handles
        else {
            int fd = fd_of(h);
            if (fd < 0) { last_error = 6; RET(0, 5); }
            k = write(fd, G + arg(c, 1), arg(c, 2));
        }
        if (arg(c, 3)) M32(arg(c, 3)) = k > 0 ? (uint32_t)k : 0;
        RET(k >= 0, 5);
    }
    F("SetFilePointer") {
        int fd = fd_of(arg(c, 0));
        if (fd < 0) { last_error = 6; RET(0xFFFFFFFF, 4); }
        int64_t dist = (int32_t)arg(c, 1);
        if (arg(c, 2)) dist = (int64_t)((uint64_t)M32(arg(c, 2)) << 32 | arg(c, 1));
        off_t r = lseek(fd, dist, arg(c, 3) == 0 ? SEEK_SET : arg(c, 3) == 1 ? SEEK_CUR : SEEK_END);
        if (arg(c, 2)) M32(arg(c, 2)) = (uint32_t)((uint64_t)r >> 32);
        RET((uint32_t)r, 4);
    }
    F("GetFileSize") {
        int fd = fd_of(arg(c, 0));
        struct stat st;
        if (fd < 0 || fstat(fd, &st)) RET(0xFFFFFFFF, 2);
        if (arg(c, 1)) M32(arg(c, 1)) = 0;
        RET((uint32_t)st.st_size, 2);
    }
    F("GetFileTime") {
        int fd = fd_of(arg(c, 0));
        struct stat st;
        if (fd < 0 || fstat(fd, &st)) RET(0, 4);
        uint64_t ft = (uint64_t)st.st_mtime * 10000000ull + 116444736000000000ull;
        for (int k = 1; k <= 3; ++k) if (arg(c, k)) M64(arg(c, k)) = ft;
        RET(1, 4);
    }
    F("SetEndOfFile") {
        int fd = fd_of(arg(c, 0));
        RET(fd >= 0 && ftruncate(fd, lseek(fd, 0, SEEK_CUR)) == 0, 1);
    }
    F("FlushFileBuffers") RET(1, 1);
    F("CreateFileMappingA") {                  // (file, sa, protect, size hi, size lo, name)
        int fd = fd_of(arg(c, 0));
        if (fd < 0) { last_error = 6; RET(0, 6); }
        struct stat st;
        fstat(fd, &st);
        for (int i = 0; i < 32; ++i)
            if (!mappings[i].used) {
                mappings[i].used = 1;
                mappings[i].fd = fd;
                mappings[i].size = (uint64_t)arg(c, 3) << 32 | arg(c, 4);
                if (!mappings[i].size) mappings[i].size = (uint64_t)st.st_size;
                trace("      CreateFileMappingA(%08X, protect %X) size %llu\n", arg(c, 0), arg(c, 2), (unsigned long long)mappings[i].size);
                RET(MAPPING_BASE + i, 6);
            }
        die("too many file mappings");
    }
    F("MapViewOfFile") {                       // (mapping, access, offset hi, offset lo, size)
        uint32_t i = arg(c, 0) - MAPPING_BASE;
        if (i >= 32 || !mappings[i].used) { last_error = 6; RET(0, 5); }
        uint64_t off = (uint64_t)arg(c, 2) << 32 | arg(c, 3);
        uint64_t size = arg(c, 4) ? arg(c, 4) : mappings[i].size - off;
        uint32_t a = map_view(mappings[i].fd, off, (uint32_t)size);
        trace("      MapViewOfFile(offset %llu, %llu bytes) -> %08X\n", (unsigned long long)off, (unsigned long long)size, a);
        RET(a, 5);
    }
    F("UnmapViewOfFile") {
        for (int i = 0; i < 64; ++i)
            if (views[i].addr == arg(c, 0)) {
                uint32_t page = (uint32_t)getpagesize(), start = views[i].addr & ~(page - 1);
                // give the range back to the reservation (readable, zero) rather than unmapping it
                mmap(G + start, views[i].size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
                views[i].addr = 0;
                RET(1, 1);
            }
        RET(0, 1);
    }
    F("FlushViewOfFile") RET(1, 2);
    F("LockFile") RET(1, 5);
    F("UnlockFile") RET(1, 5);
    F("GetFileType") RET(arg(c, 0) >= 0x100 && arg(c, 0) < 0x110 ? 2 : 1, 1);   // CHAR for std handles, DISK otherwise
    F("GetFileAttributesA") {
        char host[2048];
        struct stat st;
        path_resolve(gptr(arg(c, 0)), host, sizeof host, 0);
        if (stat(host, &st)) { last_error = 2; RET(0xFFFFFFFF, 1); }
        RET(S_ISDIR(st.st_mode) ? 0x10 : 0x80, 1);
    }
    F("CreateDirectoryA") {
        char host[2048];
        path_resolve(gptr(arg(c, 0)), host, sizeof host, 1);
        int r = mkdir(host, 0755);
        if (r && errno == EEXIST) { last_error = 183; RET(0, 2); }
        RET(r == 0, 2);
    }
    F("DeleteFileA") {
        char host[2048];
        char rel[1024];
        if (!guest_rel(gptr(arg(c, 0)), rel, sizeof rel) || !lookup(user_dir, rel, host, sizeof host)) {
            last_error = 2; RET(0, 1);                 // never delete from the game folder
        }
        RET(unlink(host) == 0, 1);
    }
    F("FindFirstFileA") {
        const char *win = gptr(arg(c, 0));
        char dirwin[1024], *slash;
        snprintf(dirwin, sizeof dirwin, "%s", win);
        for (char *p = dirwin; *p; ++p) if (*p == '/') *p = '\\';
        slash = strrchr(dirwin, '\\');
        const char *pattern = slash ? slash + 1 : dirwin;
        char pat[260];
        snprintf(pat, sizeof pat, "%s", pattern);
        if (slash) *slash = 0; else snprintf(dirwin, sizeof dirwin, ".");
        int i = 0;
        while (i < 16 && finds[i].used) ++i;
        if (i == 16) die("too many FindFirstFile handles");
        // search the user_dir copy first if it exists, else the game folder
        char rel[1024];
        if (!guest_rel(dirwin, rel, sizeof rel)) { last_error = 3; RET(0xFFFFFFFF, 2); }
        if (!lookup(user_dir, rel, finds[i].dir, sizeof finds[i].dir)) lookup(game_dir, rel, finds[i].dir, sizeof finds[i].dir);
        finds[i].d = opendir(finds[i].dir);
        trace("      FindFirstFileA(\"%s\") in %s\n", win, finds[i].dir);
        if (!finds[i].d) { last_error = 3; RET(0xFFFFFFFF, 2); }
        snprintf(finds[i].pattern, sizeof finds[i].pattern, "%s", pat);
        finds[i].used = 1;
        if (!find_next(i, arg(c, 1))) { closedir(finds[i].d); finds[i].used = 0; last_error = 2; RET(0xFFFFFFFF, 2); }
        RET(FIND_BASE + i, 2);
    }
    F("FindNextFileA") {
        uint32_t i = arg(c, 0) - FIND_BASE;
        if (i >= 16 || !finds[i].used) RET(0, 2);
        if (!find_next(i, arg(c, 1))) { last_error = 18; RET(0, 2); }   // ERROR_NO_MORE_FILES
        RET(1, 2);
    }
    F("FindClose") {
        uint32_t i = arg(c, 0) - FIND_BASE;
        if (i < 16 && finds[i].used) { closedir(finds[i].d); finds[i].used = 0; }
        RET(1, 1);
    }
    F("GetCurrentDirectoryA") {
        snprintf((char *)G + arg(c, 1), arg(c, 0), "%s", cwd);
        RET(strlen(cwd), 2);
    }
    F("GetFullPathNameA") {
        const char *win = gptr(arg(c, 0));
        char full[1024];
        if ((win[0] && win[1] == ':') || win[0] == '\\') snprintf(full, sizeof full, "%s", win);
        else snprintf(full, sizeof full, "%s\\%s", cwd, win);
        snprintf((char *)G + arg(c, 2), arg(c, 1), "%s", full);
        if (arg(c, 3)) {
            char *s = strrchr((char *)G + arg(c, 2), '\\');
            M32(arg(c, 3)) = s ? (uint32_t)(s + 1 - (char *)G) : arg(c, 2);
        }
        RET(strlen(full), 4);
    }
    F("GetDriveTypeA") RET(3, 1);                                   // DRIVE_FIXED
    F("GetLogicalDriveStringsA") {
        static const char s[] = "C:\\\0";
        memcpy(G + arg(c, 1), s, sizeof s);
        RET(4, 2);
    }
    F("GetVolumeInformationA") {
        if (arg(c, 1)) snprintf((char *)G + arg(c, 1), arg(c, 2), "PATRICIAN3");
        if (arg(c, 3)) M32(arg(c, 3)) = 0x1234ABCD;
        if (arg(c, 4)) M32(arg(c, 4)) = 255;
        if (arg(c, 5)) M32(arg(c, 5)) = 0;
        if (arg(c, 6)) snprintf((char *)G + arg(c, 6), arg(c, 7), "NTFS");
        RET(1, 8);
    }
    return 0;
}
