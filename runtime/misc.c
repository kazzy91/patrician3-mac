// misc.c - the small DLLs: ADVAPI32 (registry), WINMM (timers), SHELL32, SHLWAPI, COMCTL32, comdlg32, WINSPOOL.
// The game's own DLLs (Archiver, mss32, VTO, SchnellTransfer) get their own files when implemented.
#include <fnmatch.h>
#include <stdlib.h>

#include "host.h"

#define F(name) if (!strcmp(fn, name))
int win32_misc(Cpu *c, const char *dll, const char *fn) {
    if (!strcasecmp(dll, "ADVAPI32.dll")) return win32_advapi(c, fn);   // registry.c
    if (!strcasecmp(dll, "WINMM.dll")) {
        F("timeGetTime") RET(host_ticks() / 1000000ull, 0);
        F("timeBeginPeriod") RET(0, 1);
        F("timeEndPeriod") RET(0, 1);
        return 0;
    }
    if (!strcasecmp(dll, "COMCTL32.dll")) {
        F("#17") RET(0, 0);                       // InitCommonControls
        return 0;
    }
    if (!strcasecmp(dll, "SHLWAPI.dll")) {
        F("PathMatchSpecA") RET(fnmatch(gptr(arg(c, 1)), gptr(arg(c, 0)), FNM_CASEFOLD) == 0, 2);
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
