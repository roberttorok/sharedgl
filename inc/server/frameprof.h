#ifndef _SGL_FRAMEPROF_H_
#define _SGL_FRAMEPROF_H_

/*
 * Per-frame profile, written by the server. One CSV row per SwapBuffers, with the
 * host's time split into phases and the guest's own counters (inc/sglprof.h) next to it.
 * Turned on with SGL_FRAMEPROF. See PROFILING.md in the qemu-xp-3d repository.
 */

#include <stdint.h>
#include <stdbool.h>

#if defined(__x86_64__) || defined(__i386__)
#include <x86intrin.h>
static inline uint64_t sgl_tsc(void) { return __rdtsc(); }
#else
#include <SDL2/SDL.h>
static inline uint64_t sgl_tsc(void) { return SDL_GetPerformanceCounter(); }
#endif

enum sgl_frameprof_host_counter {
    SGL_FP_WAIT,       /* idle in wait_shm, waiting for the guest */
    SGL_FP_SETUP,      /* copying a submit out of shared memory and making the context current */
    SGL_FP_EXEC,       /* executing commands, without the readback */
    SGL_FP_ERRCHECK,   /* the glGetError() check after each command */
    SGL_FP_READBACK,   /* SwapBuffers: glReadPixels of the frame into shared memory, plus overlay */
    SGL_FP_SUBMITS,    /* count */
    SGL_FP_COMMANDS,   /* count */
    SGL_FP_BYTES,      /* bytes received */
    SGL_FP_SLEEPS,     /* sleeps taken while idle (count) */
    SGL_FP_COUNT
};

extern bool sgl_frameprof_on;
extern uint64_t sgl_frameprof_host[SGL_FP_COUNT];

void sgl_frameprof_init(void *shared);
void sgl_frameprof_command(int cmd, uint64_t cycles);
void sgl_frameprof_swap(void *shared);

#endif
