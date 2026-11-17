// mss32.c - Miles Sound System (mss32.dll) on CoreAudio.
//
// The game plays sound effects as Miles "samples" (WAV data from its archive, handed over in memory) and music and
// ambience as "streams" (MP3 files under Musik/). Everything is decoded with AudioToolbox (ExtAudioFile) to 44.1 kHz
// stereo float and mixed in one CoreAudio output callback:
//   - samples are decoded completely when the game sets their data (they are short);
//   - streams are decoded on a background thread while they play (a track is 1-5 minutes of MP3).
// Voices are guarded by one mutex shared with the mixer; Miles calls arrive under the GIL.
//
// All functions are __stdcall; the argument count comes from the "@N" in the import name.
// P3_MUTE=1 keeps everything silent (handles and decoding still work).
#include <AudioToolbox/AudioToolbox.h>
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#include "host.h"

#define RATE 44100.0
#define NVOICES 128
#define HANDLE_BASE 0x00E10000u

enum { SMP_FREE = 1, SMP_DONE = 2, SMP_PLAYING = 4, SMP_STOPPED = 8 };

typedef struct {
    int used, is_stream;
    float *pcm;                        // interleaved stereo
    uint64_t frames, cap, total;       // decoded so far, allocated, expected in all (streams: from the file length)
    uint64_t pos;
    int playing, paused, decoding, cancel;
    int loops, loops_left;             // Miles: 0 = forever
    int volume, pan;                   // 0..127, pan 64 = centre
    pthread_t thread;
    AudioFileID af;                    // streams: opened in AIL_open_stream, decoded by the thread
    ExtAudioFileRef ext;
    char path[1024];
} Voice;

static Voice voices[NVOICES];
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static AudioComponentInstance unit;
static int started, muted;
static int master_volume = 127;
static uint32_t redist_dir, last_error_str;

// ---------------- mixer ----------------
static float gain_of(int v) { float g = v / 127.0f; return g * g; }   // Miles volumes feel linear in loudness

static OSStatus render(void *ref, AudioUnitRenderActionFlags *flags, const AudioTimeStamp *ts, UInt32 bus, UInt32 n, AudioBufferList *io) {
    (void)ref; (void)flags; (void)ts; (void)bus;
    float *out = io->mBuffers[0].mData;
    memset(out, 0, (size_t)n * 2 * sizeof(float));
    if (pthread_mutex_trylock(&lock)) return noErr;      // never block the audio thread for long: skip a buffer
    float master = gain_of(master_volume);
    for (int k = 0; k < NVOICES; ++k) {
        Voice *v = &voices[k];
        if (!v->used || !v->playing || v->paused || !v->pcm) continue;
        float g = gain_of(v->volume) * master;
        float gl = g * (v->pan <= 64 ? 1.0f : (127 - v->pan) / 63.0f);
        float gr = g * (v->pan >= 64 ? 1.0f : v->pan / 64.0f);
        for (UInt32 i = 0; i < n; ++i) {
            if (v->pos >= v->frames) {
                if (v->decoding) break;                    // a stream that hasn't been decoded this far yet
                if (v->loops == 0 || --v->loops_left > 0) { v->pos = 0; if (!v->frames) break; }
                else { v->playing = 0; break; }
            }
            out[2 * i] += v->pcm[2 * v->pos] * gl;
            out[2 * i + 1] += v->pcm[2 * v->pos + 1] * gr;
            ++v->pos;
        }
    }
    pthread_mutex_unlock(&lock);
    for (UInt32 i = 0; i < 2 * n; ++i) out[i] = out[i] > 1 ? 1 : out[i] < -1 ? -1 : out[i];
    return noErr;
}

static void audio_start(void) {
    if (started) return;
    started = 1;
    muted = getenv("P3_MUTE") != NULL;
    if (muted) return;
    AudioComponentDescription d = {kAudioUnitType_Output, kAudioUnitSubType_DefaultOutput, kAudioUnitManufacturer_Apple, 0, 0};
    AudioComponent comp = AudioComponentFindNext(NULL, &d);
    if (!comp || AudioComponentInstanceNew(comp, &unit)) { fprintf(stderr, "boot: no audio output, sound is off\n"); unit = NULL; return; }
    AudioStreamBasicDescription f = {RATE, kAudioFormatLinearPCM, kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked, 8, 1, 8, 2, 32, 0};
    AudioUnitSetProperty(unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &f, sizeof f);
    AURenderCallbackStruct cb = {render, NULL};
    AudioUnitSetProperty(unit, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &cb, sizeof cb);
    if (AudioUnitInitialize(unit) || AudioOutputUnitStart(unit)) { fprintf(stderr, "boot: audio output failed to start, sound is off\n"); unit = NULL; return; }
    printf("boot: audio on (CoreAudio, 44.1 kHz stereo)\n");
    fflush(stdout);
}

// ---------------- decoding ----------------
typedef struct { const uint8_t *data; int64_t size; } Mem;
static OSStatus mem_read(void *ref, SInt64 pos, UInt32 n, void *buf, UInt32 *got) {
    Mem *m = ref;
    if (pos >= m->size) { *got = 0; return noErr; }
    if (pos + n > m->size) n = (UInt32)(m->size - pos);
    memcpy(buf, m->data + pos, n);
    *got = n;
    return noErr;
}
static SInt64 mem_size(void *ref) { return ((Mem *)ref)->size; }

static AudioFileTypeID type_hint(const char *name, const uint8_t *data) {
    if (data && !memcmp(data, "RIFF", 4)) return kAudioFileWAVEType;
    if (name) {
        const char *e = strrchr(name, '.');
        if (e && !strcasecmp(e, ".wav")) return kAudioFileWAVEType;
        if (e && !strcasecmp(e, ".mp3")) return kAudioFileMP3Type;
    }
    return kAudioFileMP3Type;
}

// Opens an ExtAudioFile with the 44.1 kHz stereo float client format; *total = output frames expected.
static ExtAudioFileRef open_ext(AudioFileID af, uint64_t *total) {
    ExtAudioFileRef ext = NULL;
    if (ExtAudioFileWrapAudioFileID(af, false, &ext)) return NULL;
    AudioStreamBasicDescription in = {0}, out = {RATE, kAudioFormatLinearPCM, kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked, 8, 1, 8, 2, 32, 0};
    UInt32 sz = sizeof in;
    ExtAudioFileGetProperty(ext, kExtAudioFileProperty_FileDataFormat, &sz, &in);
    if (ExtAudioFileSetProperty(ext, kExtAudioFileProperty_ClientDataFormat, sizeof out, &out)) { ExtAudioFileDispose(ext); return NULL; }
    SInt64 len = 0;
    sz = sizeof len;
    ExtAudioFileGetProperty(ext, kExtAudioFileProperty_FileLengthFrames, &sz, &len);
    *total = in.mSampleRate > 0 ? (uint64_t)((double)len * RATE / in.mSampleRate) : 0;
    return ext;
}

// Appends decoded audio to the voice until the file ends (or cancel). Returns frames decoded.
static uint64_t decode_into(Voice *v, ExtAudioFileRef ext) {
    float chunk[2 * 4096];
    uint64_t n = 0;
    for (;;) {
        AudioBufferList bl = {1, {{2, sizeof chunk, chunk}}};
        UInt32 frames = 4096;
        if (ExtAudioFileRead(ext, &frames, &bl) || !frames) break;
        pthread_mutex_lock(&lock);
        if (v->cancel) { pthread_mutex_unlock(&lock); break; }
        if (v->frames + frames > v->cap) {
            uint64_t cap = v->cap ? v->cap * 2 : (v->total ? v->total + 4096 : 1 << 16);
            while (cap < v->frames + frames) cap *= 2;
            float *p = realloc(v->pcm, cap * 2 * sizeof(float));
            if (!p) { pthread_mutex_unlock(&lock); break; }
            v->pcm = p;
            v->cap = cap;
        }
        memcpy(v->pcm + 2 * v->frames, chunk, (size_t)frames * 2 * sizeof(float));
        v->frames += frames;
        pthread_mutex_unlock(&lock);
        n += frames;
    }
    return n;
}

// Sample data in guest memory -> fully decoded voice. Returns 1 on success.
static int load_sample(Voice *v, const char *name, uint32_t data, uint32_t size) {
    if (!data) return 0;
    if (!size) {                                       // unknown size: a RIFF header knows it, else a generous bound
        size = !memcmp(G + data, "RIFF", 4) ? M32(data + 4) + 8 : 16u << 20;
    }
    Mem m = {G + data, size};
    AudioFileID af = NULL;
    if (AudioFileOpenWithCallbacks(&m, mem_read, NULL, mem_size, NULL, type_hint(name, G + data), &af) &&
        AudioFileOpenWithCallbacks(&m, mem_read, NULL, mem_size, NULL, 0, &af)) {
        trace("      Miles: cannot decode sample %s\n", name ? name : "(memory)");
        return 0;
    }
    uint64_t total = 0;
    ExtAudioFileRef ext = open_ext(af, &total);
    if (!ext) { AudioFileClose(af); return 0; }
    pthread_mutex_lock(&lock);
    free(v->pcm);
    v->pcm = NULL; v->frames = v->cap = 0; v->total = total; v->pos = 0; v->playing = 0; v->cancel = 0;
    pthread_mutex_unlock(&lock);
    decode_into(v, ext);
    ExtAudioFileDispose(ext);
    AudioFileClose(af);
    v->total = v->frames;
    return v->frames > 0;
}

// Opens a stream's file and reads its length (the game asks for it right after AIL_open_stream).
static int stream_open(Voice *v) {
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8 *)v->path, (CFIndex)strlen(v->path), false);
    OSStatus e = AudioFileOpenURL(url, kAudioFileReadPermission, type_hint(v->path, NULL), &v->af);
    CFRelease(url);
    if (e) { v->af = NULL; return 0; }
    uint64_t total = 0;
    v->ext = open_ext(v->af, &total);
    if (!v->ext) { AudioFileClose(v->af); v->af = NULL; return 0; }
    v->total = total;
    return 1;
}
static void *stream_thread(void *arg) {
    Voice *v = arg;
    decode_into(v, v->ext);
    ExtAudioFileDispose(v->ext);
    AudioFileClose(v->af);
    pthread_mutex_lock(&lock);
    v->total = v->frames;
    v->decoding = 0;
    pthread_mutex_unlock(&lock);
    return NULL;
}

// ---------------- handles ----------------
static Voice *voice_new(int stream) {
    pthread_mutex_lock(&lock);
    for (int k = 0; k < NVOICES; ++k)
        if (!voices[k].used) {
            Voice *v = &voices[k];
            memset(v, 0, sizeof *v);
            v->used = 1; v->is_stream = stream; v->volume = 127; v->pan = 64; v->loops = 1; v->loops_left = 1;
            pthread_mutex_unlock(&lock);
            return v;
        }
    pthread_mutex_unlock(&lock);
    return NULL;
}
static uint32_t handle_of(Voice *v) { return v ? HANDLE_BASE + (uint32_t)(v - voices) : 0; }
static Voice *voice_of(uint32_t h) {
    uint32_t k = h - HANDLE_BASE;
    return k < NVOICES && voices[k].used ? &voices[k] : NULL;
}
static void voice_free(Voice *v) {
    if (!v) return;
    pthread_mutex_lock(&lock);
    v->cancel = 1;
    v->playing = 0;
    int joinable = v->is_stream && v->thread;
    pthread_mutex_unlock(&lock);
    if (joinable) pthread_join(v->thread, NULL);       // the decoder stops at its next chunk
    pthread_mutex_lock(&lock);
    free(v->pcm);
    memset(v, 0, sizeof *v);
    pthread_mutex_unlock(&lock);
}
static int status_of(Voice *v) {
    if (!v) return SMP_DONE;
    if (v->playing) return v->paused ? SMP_STOPPED : SMP_PLAYING;
    return SMP_DONE;
}
static void ms_position(Voice *v, uint32_t total_p, uint32_t cur_p) {
    pthread_mutex_lock(&lock);
    uint64_t total = v ? (v->total > v->frames ? v->total : v->frames) : 0, pos = v ? v->pos : 0;
    pthread_mutex_unlock(&lock);
    if (total_p) M32(total_p) = (uint32_t)(total * 1000 / RATE);
    if (cur_p) M32(cur_p) = (uint32_t)(pos * 1000 / RATE);
}

static int nargs(const char *fn) {
    const char *at = strrchr(fn, '@');
    return at ? atoi(at + 1) / 4 : 0;
}

#define F(name) if (!strncmp(fn, name "@", sizeof(name)))
#define LOCKED(stmt) do { pthread_mutex_lock(&lock); stmt; pthread_mutex_unlock(&lock); } while (0)
int win32_mss32(Cpu *c, const char *fn) {
    int n = nargs(fn);
    F("_AIL_startup") RET(1, n);
    F("_AIL_shutdown") { if (unit) AudioOutputUnitStop(unit); RET(0, n); }
    F("_AIL_set_redist_directory") { uint32_t old = redist_dir; redist_dir = arg(c, 0); RET(old ? old : arg(c, 0), n); }
    F("_AIL_last_error") { if (!last_error_str) last_error_str = gstr(""); RET(last_error_str, n); }
    F("_AIL_set_preference") RET(0, n);
    F("_AIL_get_preference") RET(0, n);
    F("_AIL_waveOutOpen") {                    // (dig*, hwaveout*, device, format) -> 0 = success
        audio_start();
        M32(arg(c, 0)) = 0x00E0F000;
        if (arg(c, 1)) M32(arg(c, 1)) = 0;
        RET(0, n);
    }
    F("_AIL_waveOutClose") RET(0, n);
    F("_AIL_digital_configuration") {          // (dig, rate*, format*, string)
        if (arg(c, 1)) M32(arg(c, 1)) = 44100;
        if (arg(c, 2)) M32(arg(c, 2)) = 3;     // DIG_F_STEREO_16
        if (arg(c, 3)) strcpy((char *)G + arg(c, 3), muted ? "macOS (muted)" : "macOS CoreAudio");
        RET(0, n);
    }
    F("_AIL_set_digital_master_volume") { master_volume = arg(c, 1) > 127 ? 127 : (int)arg(c, 1); RET(0, n); }
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

    // ---- samples (sound effects) ----
    F("_AIL_allocate_sample_handle") RET(handle_of(voice_new(0)), n);
    F("_AIL_release_sample_handle") { voice_free(voice_of(arg(c, 0))); RET(0, n); }
    F("_AIL_init_sample") {
        Voice *v = voice_of(arg(c, 0));
        if (v) LOCKED(v->playing = 0; v->volume = 127; v->pan = 64; v->loops = v->loops_left = 1; v->pos = 0);
        RET(0, n);
    }
    F("_AIL_set_sample_file") {                // (s, data, block) -> 1 ok
        Voice *v = voice_of(arg(c, 0));
        RET(v && load_sample(v, NULL, arg(c, 1), 0), n);
    }
    F("_AIL_set_named_sample_file") {          // (s, name, data, size, block) -> 1 ok
        Voice *v = voice_of(arg(c, 0));
        const char *name = arg(c, 1) ? gptr(arg(c, 1)) : NULL;
        int ok = v && load_sample(v, name, arg(c, 2), arg(c, 3));
        trace("      AIL_set_named_sample_file(\"%s\", %u bytes) -> %s, %llu frames\n", name ? name : "", arg(c, 3),
              ok ? "ok" : "failed", v ? (unsigned long long)v->frames : 0ull);
        RET(ok, n);
    }
    F("_AIL_set_sample_processor") RET(0, n);
    F("_AIL_start_sample") {
        Voice *v = voice_of(arg(c, 0));
        if (v) LOCKED(v->pos = 0; v->loops_left = v->loops; v->paused = 0; v->playing = v->frames > 0);
        RET(0, n);
    }
    F("_AIL_stop_sample") { Voice *v = voice_of(arg(c, 0)); if (v) LOCKED(v->paused = 1); RET(0, n); }
    F("_AIL_end_sample") { Voice *v = voice_of(arg(c, 0)); if (v) LOCKED(v->playing = 0; v->paused = 0); RET(0, n); }
    F("_AIL_sample_status") RET(status_of(voice_of(arg(c, 0))), n);
    F("_AIL_sample_volume") { Voice *v = voice_of(arg(c, 0)); RET(v ? v->volume : 0, n); }
    F("_AIL_set_sample_volume") { Voice *v = voice_of(arg(c, 0)); if (v) v->volume = (int)(arg(c, 1) > 127 ? 127 : arg(c, 1)); RET(0, n); }
    F("_AIL_sample_pan") { Voice *v = voice_of(arg(c, 0)); RET(v ? v->pan : 64, n); }
    F("_AIL_set_sample_pan") { Voice *v = voice_of(arg(c, 0)); if (v) v->pan = (int)(arg(c, 1) > 127 ? 127 : arg(c, 1)); RET(0, n); }
    F("_AIL_sample_ms_position") { ms_position(voice_of(arg(c, 0)), arg(c, 1), arg(c, 2)); RET(0, n); }

    // ---- streams (music, ambience) ----
    F("_AIL_open_stream") {                    // (dig, filename, stream memory) -> HSTREAM or 0
        const char *win = gptr(arg(c, 1));
        char host[2048];
        if (!win || !path_resolve(win, host, sizeof host, 0) || access(host, R_OK)) {
            trace("      AIL_open_stream(\"%s\"): not found\n", win ? win : "");
            RET(0, n);
        }
        Voice *v = voice_new(1);
        if (!v) RET(0, n);
        snprintf(v->path, sizeof v->path, "%s", host);
        if (!stream_open(v)) { fprintf(stderr, "boot: Miles: cannot decode %s\n", host); voice_free(v); RET(0, n); }
        v->decoding = 1;
        if (pthread_create(&v->thread, NULL, stream_thread, v)) { v->decoding = 0; voice_free(v); RET(0, n); }
        trace("      AIL_open_stream(\"%s\") -> %08X\n", win, handle_of(v));
        RET(handle_of(v), n);
    }
    F("_AIL_close_stream") { voice_free(voice_of(arg(c, 0))); RET(0, n); }
    F("_AIL_start_stream") {
        Voice *v = voice_of(arg(c, 0));
        if (v) LOCKED(v->pos = 0; v->loops_left = v->loops; v->paused = 0; v->playing = 1);
        RET(0, n);
    }
    F("_AIL_pause_stream") { Voice *v = voice_of(arg(c, 0)); if (v) LOCKED(v->paused = arg(c, 1) != 0); RET(0, n); }
    F("_AIL_service_stream") RET(0, n);
    F("_AIL_stream_status") {
        Voice *v = voice_of(arg(c, 0));
        if (!v) RET(SMP_DONE, n);
        int s;
        LOCKED(s = v->playing ? (v->paused ? SMP_STOPPED : SMP_PLAYING) : SMP_DONE);
        RET(s, n);
    }
    F("_AIL_stream_volume") { Voice *v = voice_of(arg(c, 0)); RET(v ? v->volume : 0, n); }
    F("_AIL_set_stream_volume") { Voice *v = voice_of(arg(c, 0)); if (v) v->volume = (int)(arg(c, 1) > 127 ? 127 : arg(c, 1)); RET(0, n); }
    F("_AIL_set_stream_loop_count") {
        Voice *v = voice_of(arg(c, 0));
        if (v) LOCKED(v->loops = (int)arg(c, 1); v->loops_left = v->loops);
        RET(0, n);
    }
    F("_AIL_set_stream_ms_position") {
        Voice *v = voice_of(arg(c, 0));
        if (v) LOCKED(v->pos = (uint64_t)arg(c, 1) * (uint64_t)RATE / 1000);
        RET(0, n);
    }
    F("_AIL_stream_ms_position") { ms_position(voice_of(arg(c, 0)), arg(c, 1), arg(c, 2)); RET(0, n); }

    // ---- filters: none ----
    F("_AIL_enumerate_filters") RET(0, n);
    F("_AIL_open_filter") RET(0, n);
    F("_AIL_enumerate_filter_sample_attributes") RET(0, n);
    F("_AIL_set_filter_sample_preference") RET(0, n);
    return 0;
}
