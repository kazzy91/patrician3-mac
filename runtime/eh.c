// eh.c - hardware exceptions caught by C++ catch(...), as on Windows with Visual C++ 6.
//
// VC6's catch(...) also catches structured exceptions such as an integer divide by zero, and Patrician III relies on
// it: some of its button animations divide by timer times that are 0 (scripts\buttons.ini is missing from the game
// data), and a catch(...) further up abandons the half-built object. This file reproduces what the C++ runtime's
// __CxxFrameHandler does in that case:
//   1. walk the SEH chain from fs:[0] for a C++ frame whose current state is inside a try block with a catch(...);
//   2. unwind: run the destructors (unwind map actions) of every frame above it, and of that frame down to the try;
//   3. call the catch block with the frame's ebp, and take the continuation address it returns;
//   4. continue there with the frame's esp and ebp.
// Step 4 needs the host stack to go back to the function that owns the frame: recomp/p3_build.py makes every
// function with a catch(...) resumable (eh_push / _setjmp around its body, its continuations as entry labels), and
// eh_raise() _longjmps into it.
#include <setjmp.h>

#include "host.h"

typedef struct EhFrame { struct EhFrame *prev; uint32_t record, resume; jmp_buf jb; } EhFrame;
static __thread EhFrame *top;

void eh_push(EhFrame *f, uint32_t record) { f->prev = top; f->record = record; f->resume = 0; top = f; }
void eh_pop(EhFrame *f) { top = f->prev; }

// FuncInfo of a C++ frame handler (`mov eax, FuncInfo; jmp __CxxFrameHandler`), or 0.
static uint32_t func_info(uint32_t handler) {
    if (handler < 0x10000 || handler >= 0xF0000000u) return 0;
    if (M8(handler) != 0xB8 || M8(handler + 5) != 0xE9) return 0;
    uint32_t fi = M32(handler + 1);
    return M32(fi) == 0x19930520 ? fi : 0;
}

// Runs the unwind actions of a C++ frame from its current state down to `to`.
static void unwind_frame(Cpu *c, uint32_t record, uint32_t fi, int32_t to) {
    int32_t state = (int32_t)M32(record + 8);
    uint32_t max = M32(fi + 4), map = M32(fi + 8);
    uint32_t ebp = c->ebp;
    while (state > to && state >= 0 && (uint32_t)state < max) {
        int32_t next = (int32_t)M32(map + 8 * (uint32_t)state);
        uint32_t action = M32(map + 8 * (uint32_t)state + 4);
        M32(record + 8) = (uint32_t)next;
        if (action) {
            trace("  eh: unwind action %08X (frame %08X, state %d)\n", action, record, state);
            c->ebp = record + 12;
            call_guest(c, action, 0, NULL);
        }
        state = next;
    }
    c->ebp = ebp;
}

// Returns 0 when no catch(...) takes the exception (the caller then carries on as before); does not return otherwise.
int eh_raise(Cpu *c, uint32_t code, uint32_t at) {
    for (uint32_t r = M32(c->fs_base); r && r != 0xFFFFFFFF; r = M32(r)) {
        uint32_t fi = func_info(M32(r + 4));
        if (!fi) continue;
        int32_t state = (int32_t)M32(r + 8);
        uint32_t ntry = M32(fi + 12), tmap = M32(fi + 16);
        for (uint32_t t = 0; t < ntry; ++t) {
            uint32_t e = tmap + 20 * t;
            int32_t lo = (int32_t)M32(e), hi = (int32_t)M32(e + 4);
            if (state < lo || state > hi) continue;
            uint32_t ncatch = M32(e + 12), harr = M32(e + 16), handler = 0;
            for (uint32_t k = 0; k < ncatch && !handler; ++k)
                if (M32(harr + 16 * k + 4) == 0) handler = M32(harr + 16 * k + 12);   // catch(...)
            if (!handler) continue;
            EhFrame *f = top;
            while (f && f->record != r) f = f->prev;
            if (!f) {
                fprintf(stderr, "boot: exception %08X at %08X: catch(...) at %08X found, but its function isn't resumable; continuing\n", code, at, handler);
                return 0;
            }
            // 2. unwind the frames above, then this one down to the try block
            for (uint32_t u = M32(c->fs_base); u != r; u = M32(u)) {
                uint32_t ufi = func_info(M32(u + 4));
                if (ufi) unwind_frame(c, u, ufi, -1);
            }
            M32(c->fs_base) = r;
            unwind_frame(c, r, fi, lo);
            M32(r + 8) = (uint32_t)(hi + 1);
            // 3. the catch block, with the frame's ebp and esp
            c->ebp = r + 12;
            c->esp = M32(r - 4);
            uint32_t cont = call_guest(c, handler, 0, NULL);
            // 4. continue in the owning function
            c->ebp = r + 12;
            c->esp = M32(r - 4);
            M32(c->fs_base) = r;
            fprintf(stderr, "boot: exception %08X at %08X caught by catch(...) at %08X, continuing at %08X\n", code, at, handler, cont);
            f->resume = cont;
            top = f;
            _longjmp(f->jb, 1);
        }
    }
    return 0;
}
