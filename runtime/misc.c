// misc.c - the small DLLs: ADVAPI32 (registry), WINMM (timers), SHELL32, SHLWAPI, COMCTL32, comdlg32, WINSPOOL.
// The game's own DLLs (Archiver, mss32, VTO, SchnellTransfer) get their own files when implemented.
#include <fcntl.h>
#include <fnmatch.h>
#include <unistd.h>
#include <stdlib.h>

#include "host.h"

#define F(name) if (!strcmp(fn, name))
int win32_misc(Cpu *c, const char *dll, const char *fn) {
    if (!strcasecmp(dll, "ADVAPI32.dll")) return win32_advapi(c, fn);   // registry.c
    if (!strcasecmp(dll, "WINMM.dll")) {
        F("timeGetTime") RET(host_ticks() / 1000000ull, 0);
        F("timeBeginPeriod") RET(0, 1);
        F("timeEndPeriod") RET(0, 1);
        // Multimedia file I/O (VTO.dll reads its .sks videos with it): plain files.
        F("mmioOpenA") {                       // (name, MMIOINFO*, flags)
            char host[2048];
            path_resolve(gptr(arg(c, 0)), host, sizeof host, 0);
            int fd = open(host, O_RDONLY);
            trace("      mmioOpenA(\"%s\") -> %s\n", gptr(arg(c, 0)), fd < 0 ? "not found" : host);
            if (fd < 0) RET(0, 3);
            RET(0x00F00000 + fd, 3);
        }
        F("mmioRead") { ssize_t k = read((int)(arg(c, 0) - 0x00F00000), G + arg(c, 1), arg(c, 2)); RET(k, 3); }
        F("mmioSeek") { off_t r = lseek((int)(arg(c, 0) - 0x00F00000), (int32_t)arg(c, 1), arg(c, 2) == 0 ? SEEK_SET : arg(c, 2) == 1 ? SEEK_CUR : SEEK_END); RET(r, 3); }
        F("mmioClose") { close((int)(arg(c, 0) - 0x00F00000)); RET(0, 2); }
        return 0;
    }
    if (!strcasecmp(dll, "COMCTL32.dll")) {
        F("#17") RET(0, 0);                       // InitCommonControls
        return 0;
    }
    if (!strcasecmp(dll, "SHLWAPI.dll")) {
        F("PathMatchSpecA") {                  // (file, spec): Windows wildcards; '\\' is a path separator, not an escape
            char spec[1024];
            snprintf(spec, sizeof spec, "%s", gptr(arg(c, 1)));
            int hit = 0;
            for (char *p = strtok(spec, ";"); p && !hit; p = strtok(NULL, ";")) {
                while (*p == ' ') ++p;
                hit = fnmatch(p, gptr(arg(c, 0)), FNM_CASEFOLD | FNM_NOESCAPE) == 0;
            }
            RET(hit, 2);
        }
        return 0;
    }
    if (!strcasecmp(dll, "comdlg32.dll")) {
        F("GetFileTitleA") {
            const char *p = gptr(arg(c, 0)), *s = strrchr(p, '\\');
            snprintf((char *)G + arg(c, 1), (uint16_t)arg(c, 2), "%s", s ? s + 1 : p);
            RET(0, 3);
        }
        return 0;
    }
    return 0;
}
