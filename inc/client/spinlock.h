#ifndef _SPINLOCK_H_
#define _SPINLOCK_H_

#ifndef SGL_INTERNAL
#ifdef _WIN32
#define SGL_INTERNAL __cdecl /* glimpl.c is built with -mrtd so that GL entry points are stdcall */
#else
#define SGL_INTERNAL
#endif
#endif


void SGL_INTERNAL spin_pause(void);
void SGL_INTERNAL spin_init(void);
void SGL_INTERNAL spin_fini(void);
void SGL_INTERNAL spin_backoff(unsigned int *count);
unsigned int SGL_INTERNAL spin_ticks(void);
void SGL_INTERNAL spin_lock(int *lock);
void SGL_INTERNAL spin_unlock(int volatile *lock);

#endif
