#ifndef _SGL_GLIMPL_H_
#define _SGL_GLIMPL_H_

#ifndef SGL_INTERNAL
#ifdef _WIN32
#define SGL_INTERNAL __cdecl /* glimpl.c is built with -mrtd so that GL entry points are stdcall */
#else
#define SGL_INTERNAL
#endif
#endif


#include <commongl.h>

#include <stdbool.h>

void SGL_INTERNAL glimpl_init();
bool SGL_INTERNAL glimpl_is_ready();
void SGL_INTERNAL glimpl_note_stub(const char *name);
void SGL_INTERNAL glimpl_submit();
void SGL_INTERNAL glimpl_goodbye();
void SGL_INTERNAL glimpl_report(int width, int height);
void SGL_INTERNAL glimpl_swap_buffers(int width, int height, int vflip, int format);
void *SGL_INTERNAL glimpl_fb_address();

/*
 * gl... functions don't need to be public, however
 * for windows icd we seemingly need to get a proc
 * table for 1.1
 */
#ifdef _WIN32
#include <client/platform/gldrv.h>
PGLCLTPROCTABLE SGL_INTERNAL glimpl_GetProcTable();
#endif

#endif