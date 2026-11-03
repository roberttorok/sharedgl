#ifndef _SCRATCH_H_
#define _SCRATCH_H_

#ifndef SGL_INTERNAL
#ifdef _WIN32
#define SGL_INTERNAL __cdecl /* glimpl.c is built with -mrtd so that GL entry points are stdcall */
#else
#define SGL_INTERNAL
#endif
#endif


#include <stddef.h>

void *SGL_INTERNAL scratch_buffer_get(size_t size);

#endif