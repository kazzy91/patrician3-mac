// mss32.c - Miles Sound System (mss32.dll), silent for now.
//
// The game uses Miles for music streams and sound samples. This layer gives it working handles and file loading,
// plays nothing, and reports every sample and stream as finished. CoreAudio playback replaces the silent parts later.
// All functions are __stdcall; the argument count comes from the "@N" in the import name.
#include <fcntl.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#include "host.h"

static uint32_t next_handle = 0x00E00000;
static uint32_t redist_dir, last_error_str;

static int nargs(const char *fn) {
    const char *at = strrchr(fn, '@');
    return at ? atoi(at + 1) / 4 : 0;
}

#define F(name) if (!strncmp(fn, name "@", sizeof(name)))
int win32_mss32(Cpu *c, const char *fn) {
    int n = nargs(fn);
    F("_AIL_startup") RET(1, n);
    F("_AIL_shutdown") RET(0, n);
    F("_AIL_set_redist_directory") { uint32_t old = redist_dir; redist_dir = arg(c, 0); RET(old ? old : arg(c, 0), n); }
    F("_AIL_last_error") { if (!last_error_str) last_error_str = gstr(""); RET(last_error_str, n); }
    F("_AIL_set_preference") RET(0, n);
    F("_AIL_get_preference") RET(0, n);
    F("_AIL_waveOutOpen") {                    // (dig*, hwaveout*, device, format) -> 0 = success
        M32(arg(c, 0)) = ++next_handle;
        if (arg(c, 1)) M32(arg(c, 1)) = 0;
        RET(0, n);
    }
    F("_AIL_waveOutClose") RET(0, n);
    F("_AIL_digital_configuration") {          // (dig, rate*, format*, string)
        if (arg(c, 1)) M32(arg(c, 1)) = 22050;
        if (arg(c, 2)) M32(arg(c, 2)) = 3;     // DIG_F_STEREO_16
        if (arg(c, 3)) strcpy((char *)G + arg(c, 3), "macOS (silent)");
        RET(0, n);
    }
    F("_AIL_set_digital_master_volume") RET(0, n);
    F("_AIL_mem_alloc_lock") RET(heap_alloc(arg(c, 0), 1), n);
    F("_AIL_mem_free_lock") { heap_free(arg(c, 0)); RET(0, n); }
    F("_AIL_file_read") {                      // (filename, dest or 0) -> data
        char host[2048];
        path_resolve(gptr(arg(c, 0)), host, sizeof host, 0);
        int fd = open(host, O_RDONLY);
        if (fd < 0) { trace("      AIL_file_read(\"%s\"): not found\n", gptr(arg(c, 0))); RET(0, n); }
        struct stat st;
        fstat(fd, &st);
        uint32_t dest = arg(c, 1) && arg(c, 1) != 0xFFFFFFFF ? arg(c, 1) : heap_alloc((uint32_t)st.st_size + 4, 0);
        ssize_t k = read(fd, G + dest, st.st_size);
        close(fd);
        (void)k;
        RET(dest, n);
    }
    // samples
    F("_AIL_allocate_sample_handle") RET(++next_handle, n);
    F("_AIL_release_sample_handle") RET(0, n);
    F("_AIL_init_sample") RET(0, n);
    F("_AIL_set_sample_file") RET(1, n);
    F("_AIL_set_named_sample_file") RET(1, n);
    F("_AIL_set_sample_processor") RET(0, n);
    F("_AIL_start_sample") RET(0, n);
    F("_AIL_stop_sample") RET(0, n);
    F("_AIL_end_sample") RET(0, n);
    F("_AIL_sample_status") RET(2, n);         // SMP_DONE
    F("_AIL_sample_volume") RET(127, n);
    F("_AIL_set_sample_volume") RET(0, n);
    F("_AIL_sample_pan") RET(64, n);
    F("_AIL_set_sample_pan") RET(0, n);
    F("_AIL_sample_ms_position") { if (arg(c, 1)) M32(arg(c, 1)) = 0; if (arg(c, 2)) M32(arg(c, 2)) = 0; RET(0, n); }
    // streams
    F("_AIL_open_stream") { trace("      AIL_open_stream(\"%s\")\n", gptr(arg(c, 1))); RET(++next_handle, n); }
    F("_AIL_close_stream") RET(0, n);
    F("_AIL_start_stream") RET(0, n);
    F("_AIL_pause_stream") RET(0, n);
    F("_AIL_service_stream") RET(0, n);
    F("_AIL_stream_status") RET(2, n);         // SMP_DONE
    F("_AIL_stream_volume") RET(127, n);
    F("_AIL_set_stream_volume") RET(0, n);
    F("_AIL_set_stream_loop_count") RET(0, n);
    F("_AIL_set_stream_ms_position") RET(0, n);
    F("_AIL_stream_ms_position") { if (arg(c, 1)) M32(arg(c, 1)) = 0; if (arg(c, 2)) M32(arg(c, 2)) = 0; RET(0, n); }
    // filters: none
    F("_AIL_enumerate_filters") RET(0, n);
    F("_AIL_open_filter") RET(0, n);
    F("_AIL_enumerate_filter_sample_attributes") RET(0, n);
    F("_AIL_set_filter_sample_preference") RET(0, n);
    return 0;
}
