#include <client/spinlock.h>
#if defined(__x86_64__) || defined(_M_X64) || defined(i386) || defined(__i386__) || defined(__i386) || defined(_M_IX86)
#include <emmintrin.h>
#else
// #warning Non-x86 platform detected, assuming ARM.
#define _mm_pause() asm volatile("yield");
#endif

#ifdef _WIN32
#include <windows.h>
#include <intrin.h>
#include <mmsystem.h>

/* QueryPerformanceCounter can read an emulated timer on XP. Calling it on
 * every poll makes the poll itself a VM exit. Calibrate TSC once instead. */
static unsigned int spin_budget;
static int original_wait;

void spin_init(void)
{
    LARGE_INTEGER frequency, begin, end;
    unsigned long long first, last, ticks;
    char value[16] = "";
    GetEnvironmentVariableA("SGL_WAIT_MODE", value, sizeof(value));
    original_wait = strcmp(value, "original") == 0;
    timeBeginPeriod(1);
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&begin);
    first = __rdtsc();
    Sleep(20);
    last = __rdtsc();
    QueryPerformanceCounter(&end);
    ticks = (last - first) * frequency.QuadPart / (end.QuadPart - begin.QuadPart) * 3 / 1000;
    spin_budget = ticks > 0 && ticks < 0x40000000 ? (unsigned int)ticks : 1000000;
}

void spin_fini(void)
{
    timeEndPeriod(1);
}
#else
void spin_init(void) {}
void spin_fini(void) {}
#endif

unsigned int spin_ticks(void)
{
#ifdef _WIN32
    return GetTickCount();
#else
    return 0;
#endif
}

void spin_pause(void)
{
    _mm_pause();
}

volatile unsigned int spin_sleeps;

/* For waits on the server: spin for short ones, give the CPU back for long ones. */
void spin_backoff(unsigned int *count)
{
#ifdef _WIN32
    if (!original_wait && spin_budget) {
        unsigned int now = (unsigned int)__rdtsc();
        if (*count == 0)
            *count = now ? now : 1;
        if ((unsigned int)(now - *count) < spin_budget) {
            /* PAUSE may yield the virtual CPU under a hypervisor. Poll in
             * userspace for short server replies, without an emulated timer. */
            __asm__ __volatile__("" ::: "memory");
        }
        else {
            spin_sleeps++;
            Sleep(1);
            *count = 0;
        }
        return;
    }
    if (++*count < 4000)
        _mm_pause();
    else if (*count < 4100) {
        spin_sleeps++;
        Sleep(0);
    }
    else {
        spin_sleeps++;
        Sleep(1);
    }
#else
    (void)count;
    _mm_pause();
#endif
}

void spin_lock(int *lock)
{
#ifndef _WIN32
    while (!__sync_bool_compare_and_swap(lock, 0, 1))
        _mm_pause();
#else
    while (InterlockedCompareExchange((volatile LONG*)lock, 1, 0))
        _mm_pause();
#endif
}

void spin_unlock(int volatile *lock)
{
#ifndef _WIN32
    asm volatile ("":::"memory");
#endif
    *lock = 0;
}
