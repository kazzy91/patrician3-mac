// threads.c - guest threads, TLS, critical sections, events and waits.
//
// Each guest thread is a pthread with its own Cpu, guest stack and TEB. Guest code runs one thread at a time under a
// global lock (the GIL, as in nfs-most-wanted-mac's threads.c): the host layer (heap, files, windows) then needs no
// locking of its own. The lock is handed over in FIFO order when a thread sleeps or waits, and every few hundred
// import calls if another thread is waiting, so a busy thread can't starve the others.
//
// Blocking (WaitForSingleObject, EnterCriticalSection on a held section, a suspended thread) is a loop: check the
// state while holding the GIL, otherwise give the GIL away and sleep until some state changes (or 2 ms pass).
// Only the main thread talks to Cocoa: it keeps taking window events while it waits.
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <unistd.h>

#include "host.h"

void input_pump(void);
int files_close(uint32_t h);

// ---------------- the GIL ----------------
static pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cond_gil = PTHREAD_COND_INITIALIZER, cond_obj = PTHREAD_COND_INITIALIZER;
static int gil_held;
static uint64_t next_ticket, serving;

static void lock_locked(void) {            // m held
    uint64_t my = next_ticket++;
    while (gil_held || my != serving) pthread_cond_wait(&cond_gil, &m);
    gil_held = 1;
    ++serving;
}
static void unlock_locked(void) { gil_held = 0; pthread_cond_broadcast(&cond_gil); }
void gil_acquire(void) { pthread_mutex_lock(&m); lock_locked(); pthread_mutex_unlock(&m); }
void gil_release(void) { pthread_mutex_lock(&m); unlock_locked(); pthread_mutex_unlock(&m); }
// Gives the GIL away and sleeps until an object changes state or `ns` pass, then takes the GIL again.
static void wait_change(uint64_t ns) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_nsec += (long)(ns % 1000000000ull);
    ts.tv_sec += (time_t)(ns / 1000000000ull) + ts.tv_nsec / 1000000000;
    ts.tv_nsec %= 1000000000;
    pthread_mutex_lock(&m);
    unlock_locked();
    pthread_cond_timedwait(&cond_obj, &m, &ts);
    lock_locked();
    pthread_mutex_unlock(&m);
}
static void changed(void) { pthread_cond_broadcast(&cond_obj); }

// ---------------- threads ----------------
#define NTHREADS 64
#define THREAD_BASE 0x2800u
typedef struct {
    int used, started, finished, closed;
    int suspend;
    pthread_t pt;
    Cpu cpu;
    uint32_t id, start, param, teb, stack, stack_size, exit_code;
    uint32_t tls[64];
} Thread;
static Thread threads[NTHREADS];           // [0] is the main thread
static __thread Thread *cur;
static uint64_t tls_used;

static Thread *me(void) {
    if (!cur) { cur = &threads[0]; threads[0].used = threads[0].started = 1; threads[0].id = 1; }
    return cur;
}
// After any wait: a thread another thread suspended stays parked here.
static void park_if_suspended(void) {
    Thread *t = me();
    while (t->suspend > 0) wait_change(2000000);
}
void gil_maybe_yield(void) {
    static unsigned n;
    static int off = -1;
    if (off < 0) off = getenv("P3_NO_YIELD") != NULL;   // threads then switch only when one sleeps or waits
    if (off || ++n & 0xFF) return;
    pthread_mutex_lock(&m);
    int waiters = next_ticket != serving;
    if (waiters) { unlock_locked(); lock_locked(); }
    pthread_mutex_unlock(&m);
    if (waiters) park_if_suspended();
}

static void *thread_main(void *arg) {
    Thread *t = arg;
    cur = t;
    gil_acquire();
    park_if_suspended();
    Cpu *c = &t->cpu;
    c->fs_base = t->teb;
    c->cw = 0x027F;
    c->esp = t->stack + t->stack_size - 64;
    c->esp -= 4; M32(c->esp) = t->param;
    c->esp -= 4; M32(c->esp) = 0xDEADBEEF;
    trace("  thread %u starts at %08X(%08X)\n", t->id, t->start, t->param);
    dispatch(c, t->start);
    t->exit_code = c->eax;
    t->finished = 1;
    trace("  thread %u returned %u\n", t->id, t->exit_code);
    changed();
    gil_release();
    return NULL;
}
static void thread_launch(Thread *t) {
    if (t->started) return;
    t->started = 1;
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setstacksize(&a, 64u << 20);          // translated code recurses on the host stack
    if (pthread_create(&t->pt, &a, thread_main, t)) die("pthread_create failed: %s", strerror(errno));
    pthread_attr_destroy(&a);
    pthread_detach(t->pt);
}
static Thread *thread_of(uint32_t h) {
    if (h == 0xFFFFFFFE) return me();                   // GetCurrentThread's pseudo handle
    uint32_t i = h - THREAD_BASE;
    return i < NTHREADS && threads[i].used ? &threads[i] : NULL;
}

// ---------------- events ----------------
#define EVENT_BASE 0x2000u
#define NEVENTS 256
static struct { int used, manual, state; } events[NEVENTS];

// Waits while `still_waiting()` holds; returns 0 when it stopped holding, 0x102 on timeout.
static uint32_t wait_until(int (*done)(uint32_t), uint32_t arg, uint32_t ms) {
    uint64_t deadline = ms == 0xFFFFFFFF ? UINT64_MAX : host_ticks() + (uint64_t)ms * 1000000ull;
    for (;;) {
        if (done(arg)) return 0;
        uint64_t now = host_ticks();
        if (now >= deadline) return 0x102;                  // WAIT_TIMEOUT
        uint64_t left = deadline - now;
        if (pthread_main_np()) input_pump();                // keep the window alive while the main thread waits
        wait_change(left < 2000000 ? left : 2000000);
        park_if_suspended();
    }
}
static int event_done(uint32_t i) {
    if (!events[i].state) return 0;
    if (!events[i].manual) events[i].state = 0;             // auto-reset: one waiter takes it
    return 1;
}
static int thread_done(uint32_t i) { return threads[i].finished; }
static int cs_free(uint32_t cs) { return M32(cs + 12) == 0 || M32(cs + 12) == me()->id; }

void threads_init(void) {
    me();
    gil_acquire();
}

#define F(name) if (!strcmp(fn, name))
int win32_threads(Cpu *c, const char *fn) {
    Thread *self = me();
    // ---- TLS ----
    F("TlsAlloc") {
        for (int i = 0; i < 64; ++i)
            if (!(tls_used >> i & 1)) {
                tls_used |= 1ull << i;
                for (int k = 0; k < NTHREADS; ++k) threads[k].tls[i] = 0;
                RET(i, 0);
            }
        RET(0xFFFFFFFF, 0);
    }
    F("TlsFree") { if (arg(c, 0) < 64) tls_used &= ~(1ull << arg(c, 0)); RET(1, 1); }
    F("TlsGetValue") { last_error = 0; RET(arg(c, 0) < 64 ? self->tls[arg(c, 0)] : 0, 1); }
    F("TlsSetValue") { if (arg(c, 0) < 64) self->tls[arg(c, 0)] = arg(c, 1); RET(1, 2); }

    // ---- critical sections: {debug, LockCount, RecursionCount, OwningThread, semaphore, spin} ----
    F("InitializeCriticalSection") { memset(G + arg(c, 0), 0, 24); M32(arg(c, 0) + 4) = 0xFFFFFFFF; RET(0, 1); }
    F("DeleteCriticalSection") RET(0, 1);
    F("EnterCriticalSection") {
        uint32_t cs = arg(c, 0);
        if (!cs_free(cs)) wait_until(cs_free, cs, 0xFFFFFFFF);
        M32(cs + 12) = self->id;
        M32(cs + 8) += 1;
        M32(cs + 4) += 1;
        RET(0, 1);
    }
    F("LeaveCriticalSection") {
        uint32_t cs = arg(c, 0);
        if (M32(cs + 8)) {
            M32(cs + 4) -= 1;
            if (--M32(cs + 8) == 0) { M32(cs + 12) = 0; changed(); }
        }
        RET(0, 1);
    }

    // ---- events ----
    F("CreateEventA") {                        // (sa, manual, initial, name)
        for (int i = 0; i < NEVENTS; ++i)
            if (!events[i].used) {
                events[i].used = 1; events[i].manual = arg(c, 1) != 0; events[i].state = arg(c, 2) != 0;
                RET(EVENT_BASE + i, 4);
            }
        die("too many events");
    }
    F("SetEvent") { uint32_t i = arg(c, 0) - EVENT_BASE; if (i < NEVENTS) { events[i].state = 1; changed(); } RET(1, 1); }
    F("ResetEvent") { uint32_t i = arg(c, 0) - EVENT_BASE; if (i < NEVENTS) events[i].state = 0; RET(1, 1); }

    // ---- threads ----
    F("CreateThread") {                        // (sa, stack size, start, param, flags, id*)
        int i = 1;
        while (i < NTHREADS && threads[i].used && !(threads[i].finished && threads[i].closed)) ++i;
        if (i == NTHREADS) die("too many threads");
        Thread *t = &threads[i];
        memset(t, 0, sizeof *t);
        t->used = 1;
        t->id = 2 + (uint32_t)i;
        t->start = arg(c, 2); t->param = arg(c, 3);
        t->stack_size = arg(c, 1) > (1u << 20) ? (arg(c, 1) + 0xFFFF) & ~0xFFFFu : 1u << 20;
        t->stack = heap_pages(t->stack_size);
        t->teb = heap_pages(0x1000);
        M32(t->teb + 0x00) = 0xFFFFFFFF;       // SEH chain end
        M32(t->teb + 0x04) = t->stack + t->stack_size;
        M32(t->teb + 0x08) = t->stack;
        M32(t->teb + 0x18) = t->teb;
        M32(t->teb + 0x20) = 1;
        M32(t->teb + 0x24) = t->id;
        M32(t->teb + 0x30) = PEB;
        t->suspend = arg(c, 4) & 4 ? 1 : 0;    // CREATE_SUSPENDED
        if (arg(c, 5)) M32(arg(c, 5)) = t->id;
        printf("boot: thread %u created (start %08X, param %08X%s)\n", t->id, t->start, t->param, t->suspend ? ", suspended" : "");
        fflush(stdout);
        if (!t->suspend) thread_launch(t);
        RET(THREAD_BASE + i, 6);
    }
    F("ResumeThread") {
        Thread *t = thread_of(arg(c, 0));
        if (!t) RET(0xFFFFFFFF, 1);
        uint32_t prev = (uint32_t)t->suspend;
        if (t->suspend > 0 && --t->suspend == 0) { thread_launch(t); changed(); }
        RET(prev, 1);
    }
    F("SuspendThread") {
        Thread *t = thread_of(arg(c, 0));
        if (!t) RET(0xFFFFFFFF, 1);
        uint32_t prev = (uint32_t)t->suspend++;
        if (t == self) { c->eax = prev; c->esp += 8; park_if_suspended(); return 1; }
        RET(prev, 1);
    }
    F("SetThreadPriority") RET(1, 2);
    F("GetCurrentThread") RET(0xFFFFFFFE, 0);
    F("GetCurrentThreadId") RET(self->id, 0);
    F("ExitThread") {
        if (self == &threads[0]) die("ExitThread(%u) on the main thread", arg(c, 0));
        self->exit_code = arg(c, 0);
        self->finished = 1;
        trace("  thread %u exits with %u\n", self->id, self->exit_code);
        changed();
        gil_release();
        pthread_exit(NULL);
    }

    // ---- waits ----
    F("WaitForSingleObject") {                 // (handle, ms)
        uint32_t h = arg(c, 0), ms = arg(c, 1), r;
        if (h - EVENT_BASE < NEVENTS && events[h - EVENT_BASE].used) r = wait_until(event_done, h - EVENT_BASE, ms);
        else if (thread_of(h)) r = wait_until(thread_done, (uint32_t)(thread_of(h) - threads), ms);
        else { trace("      WaitForSingleObject(%08X): not a thread or event, returns at once\n", h); r = 0; }
        RET(r, 2);
    }
    F("Sleep") {
        uint32_t ms = arg(c, 0);
        if (pthread_main_np()) input_pump();
        gil_release();
        if (ms) usleep(ms * 1000u); else sched_yield();
        gil_acquire();
        park_if_suspended();
        RET(0, 1);
    }
    F("CloseHandle") {
        uint32_t h = arg(c, 0);
        if (h - EVENT_BASE < NEVENTS) { events[h - EVENT_BASE].used = 0; RET(1, 1); }
        if (h - THREAD_BASE < NTHREADS) { threads[h - THREAD_BASE].closed = 1; RET(1, 1); }   // the slot is reused once the thread has ended
        return 0;                                           // files and the rest: kernel32.c
    }
    return 0;
}
