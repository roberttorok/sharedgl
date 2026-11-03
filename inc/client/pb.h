#ifndef _SGL_PB_H_
#define _SGL_PB_H_

#ifndef SGL_INTERNAL
#ifdef _WIN32
#define SGL_INTERNAL __cdecl /* glimpl.c is built with -mrtd so that GL entry points are stdcall */
#else
#define SGL_INTERNAL
#endif
#endif


#ifndef _WIN32
#include <fcntl.h>
#include <stddef.h>
#include <unistd.h>
#endif

#include <stdint.h>
#include <stdbool.h>

struct pb_net_hooks {
    int(*_pb_read)(int s);
    int64_t(*_pb_read64)(int s);
    void(*_pb_write)(int s, int c);
    void(*_pb_copy)(void *data, int s, size_t length);
    void(*_pb_memcpy)(void *src, size_t length);
    void*(*_pb_ptr)(size_t offs);
    void(*_pb_copy_to_shared)();
};

void SGL_INTERNAL pb_set_net(struct pb_net_hooks hooks, size_t internal_alloc_size);

#ifndef _WIN32
/*
 * to-do: determine if direct_access is really unneeded
 */
void SGL_INTERNAL pb_set(int pb, bool direct_access);
#else
bool SGL_INTERNAL pb_set(bool direct_access);
void SGL_INTERNAL pb_unset(void);
#endif

void SGL_INTERNAL pb_reset();
void SGL_INTERNAL pb_push(int c);
void SGL_INTERNAL pb_push64(int64_t c);
void SGL_INTERNAL pb_pushf(float c);

int SGL_INTERNAL pb_read(int s);
int64_t SGL_INTERNAL pb_read64(int s);
void SGL_INTERNAL pb_write(int s, int c);
int SGL_INTERNAL pb_global_read(int s);
int64_t SGL_INTERNAL pb_global_read64(int s);
void SGL_INTERNAL pb_global_write(int s, int c);

void SGL_INTERNAL pb_memcpy(const void *src, size_t length);
void SGL_INTERNAL pb_memcpy_unaligned(const void *src, size_t length);
void SGL_INTERNAL pb_realign();

void *SGL_INTERNAL pb_ptr(size_t offs);
void *SGL_INTERNAL pb_global_ptr(size_t offs);
void SGL_INTERNAL pb_set_client(int client_id);

/*
 * special case: return pointer within internal space
 * only used for networking junk
 * basically, todo: organize everything !!
 */
void *SGL_INTERNAL pb_iptr(size_t offs);

size_t SGL_INTERNAL pb_size();
size_t SGL_INTERNAL pb_capacity();
bool SGL_INTERNAL pb_overflowed();

void SGL_INTERNAL pb_copy_to_shared();

#endif
