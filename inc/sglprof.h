#ifndef _SGLPROF_H_
#define _SGLPROF_H_

/*
 * Per-frame profiling shared between the XP client (opengl32.dll) and the host server.
 * See PROFILING.md in the qemu-xp-3d repository.
 *
 * The server turns it on by writing SGL_PROF_MAGIC into `magic`. The client only records
 * while it sees the magic, so nothing has to be configured inside the guest.
 *
 * Every counter is 32 bits so that a 32-bit guest writes it atomically. They only ever
 * grow and wrap around; the server takes differences between frames.
 * Times are TSC cycles. Under KVM and WHPX the guest TSC runs at the host's rate, which
 * the server checks and reports as tsc_ratio.
 */

#include <stdint.h>

#define SGL_PROF_OFFSET  0x800   /* in the global register page, which is otherwise unused past 0x54 */
#define SGL_PROF_MAGIC   0x50474c53u /* "SLGP" */
#define SGL_PROF_VERSION 1

/* Experiments only: they change what the guest does, to measure what a part costs. */
#define SGL_PROF_CTL_NO_BLIT 1  /* skip the GDI blit to the window: the screen stops updating */

enum sgl_prof_guest_counter {
    SGL_PROF_G_SUBMIT_WAIT,     /* waiting for the server to finish a submit (includes the host's work) */
    SGL_PROF_G_SUBMIT_LOCK,     /* waiting for the shared staging area to be free */
    SGL_PROF_G_SUBMIT_COPY,     /* copying the command buffer into shared memory */
    SGL_PROF_G_SUBMITS,         /* count */
    SGL_PROF_G_BYTES,           /* bytes submitted */
    SGL_PROF_G_SLEEPS,          /* Sleep() calls while waiting for the server (count) */
    SGL_PROF_G_SWAP_COPY,       /* in SwapBuffers after the frame came back: copy or blit of the frame */
    SGL_PROF_G_PACE,            /* frame-rate limiter (sglrenderer -f) */
    SGL_PROF_G_PRESENT_THREAD,  /* present thread: SetDIBitsToDevice of a finished frame (runs in parallel) */
    SGL_PROF_G_PRESENTS,        /* frames the present thread showed (count) */
    SGL_PROF_G_COUNT
};

struct sgl_prof_shared {
    volatile uint32_t magic;          /* host: SGL_PROF_MAGIC while recording */
    volatile uint32_t version;        /* host: SGL_PROF_VERSION */
    volatile uint32_t guest_version;  /* guest: SGL_PROF_VERSION once it records */
    volatile uint32_t guest_rdtsc;    /* guest: cycles for one RDTSC, measured once */
    volatile uint32_t swap_tsc_lo;    /* guest: TSC when SwapBuffers was entered */
    volatile uint32_t swap_tsc_hi;
    volatile uint32_t control;        /* host: SGL_PROF_CTL_* experiment switches (SGL_FRAMEPROF_CONTROL) */
    volatile uint32_t reserved[9];
    volatile uint32_t counter[SGL_PROF_G_COUNT];
};

#endif
