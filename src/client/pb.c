#include <sharedgl.h>
#include <client/pb.h>

#include <stdio.h>
#include <string.h>

#ifndef _WIN32
#define __USE_GNU
#define _GNU_SOURCE
#include <sys/mman.h>
#else
#include <windows.h>
#include <setupapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <malloc.h>

#include <initguid.h>

#pragma comment (lib, "Setupapi.lib")

DEFINE_GUID(GUID_DEVINTERFACE_IVSHMEM,
    0xdf576976, 0x569d, 0x4672, 0x95, 0xa0, 0xf5, 0x7e, 0x4e, 0xa0, 0xb2, 0x10);

#define IVSHMEM_CACHE_NONCACHED     0
#define IVSHMEM_CACHE_CACHED        1
#define IVSHMEM_CACHE_WRITECOMBINED 2

#define IOCTL_IVSHMEM_REQUEST_SIZE   CTL_CODE(FILE_DEVICE_UNKNOWN, 0x801, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_IVSHMEM_REQUEST_MMAP   CTL_CODE(FILE_DEVICE_UNKNOWN, 0x802, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_IVSHMEM_RELEASE_MMAP   CTL_CODE(FILE_DEVICE_UNKNOWN, 0x803, METHOD_BUFFERED, FILE_ANY_ACCESS)

struct ivshmem_mmap_config {
    BYTE cache_mode;
};

struct ivshmem_mmap {
    WORD peer_id;
    DWORD64 size;
    PVOID pointer;
    WORD vectors;
};

static HANDLE ivshmem_handle = INVALID_HANDLE_VALUE;
#endif

static void *ptr;
static void *base;
static int *cur;

static void *in_base;
static int *in_cur;
static size_t in_capacity;
static size_t client_slot_offset;
static bool has_client_slot = false;
static bool write_overflowed = false;

static bool using_direct_access = false;

static struct pb_net_hooks net_hooks = { NULL };

static inline bool pb_is_global_offset(size_t offs)
{
    switch (offs) {
    case SGL_OFFSET_REGISTER_READY_HINT:
    case SGL_OFFSET_REGISTER_LOCK:
    case SGL_OFFSET_REGISTER_CLAIM_ID:
    case SGL_OFFSET_REGISTER_CONNECT:
    case SGL_OFFSET_REGISTER_FBSTART:
    case SGL_OFFSET_REGISTER_MEMSIZE:
    case SGL_OFFSET_REGISTER_SWAP_BUFFERS_SYNC:
    case SGL_OFFSET_REGISTER_GLMAJ:
    case SGL_OFFSET_REGISTER_GLMIN:
    case SGL_OFFSET_REGISTER_STAGE_CLIENT_ID:
    case SGL_OFFSET_REGISTER_STAGE_SIZE:
    case SGL_OFFSET_REGISTER_FIFO_SIZE:
    case SGL_OFFSET_REGISTER_MAX_CLIENTS:
        return true;
    default:
        return false;
    }
}

static inline size_t pb_resolve_offset(size_t offs)
{
    if (has_client_slot && offs < SGL_CLIENT_SLOT_SIZE && !pb_is_global_offset(offs))
        return client_slot_offset + offs;
    return offs;
}

static inline bool pb_can_write(size_t length)
{
    size_t used = (size_t)((char*)in_cur - (char*)in_base);
    return used + length <= in_capacity;
}

static void pb_note_overflow(size_t length)
{
    if (!write_overflowed)
        PRINT_LOG("push buffer overflow: used=%zu requested=%zu capacity=%zu\n",
            (size_t)((char*)in_cur - (char*)in_base), length, in_capacity);
    write_overflowed = true;
}

#ifndef _WIN32
void pb_set(int fd, bool direct_access)
{
    uintptr_t alloc_size;
    int fifo_size;

    ptr = mmap(NULL, 0x1000, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    alloc_size = *(uintptr_t*)((char*)ptr + SGL_OFFSET_REGISTER_MEMSIZE);
    fifo_size = *(int*)((char*)ptr + SGL_OFFSET_REGISTER_FIFO_SIZE);
    
    munmap(ptr, 0x1000);
    ptr = mmap(NULL, alloc_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);

    base = (char*)ptr + SGL_STAGE_OFFSET;
    in_capacity = (size_t)fifo_size;
    using_direct_access = direct_access;
    has_client_slot = false;
    write_overflowed = false;

    if (direct_access) {
        in_base = base;
        in_cur = base;
    }
    else {
        in_base = mmap(NULL, in_capacity, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        in_cur = in_base;
    }
}
#else
bool pb_set(bool direct_access)
{
    HDEVINFO device_info;
    PSP_DEVICE_INTERFACE_DETAIL_DATA_A inf_data;
    SP_DEVICE_INTERFACE_DATA dev_data;
    DWORD64 size;
    int fifo_size;
    struct ivshmem_mmap_config config;
    struct ivshmem_mmap map;
    DWORD request_size = 0;
    DWORD returned = 0;
    HANDLE handle;

    /*
     * SetupDiGetClassDevs(NULL, ..., DIGCF_ALLCLASSES | DIGCF_DEVICEINTERFACE)
     * finds nothing on Windows XP, so ask for the interface class directly.
     */
    device_info = SetupDiGetClassDevsA(&GUID_DEVINTERFACE_IVSHMEM, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (device_info == INVALID_HANDLE_VALUE)
        return false;

    ZeroMemory(&dev_data, sizeof(SP_DEVICE_INTERFACE_DATA));
    dev_data.cbSize = sizeof(SP_DEVICE_INTERFACE_DATA);

    if (SetupDiEnumDeviceInterfaces(device_info, NULL, &GUID_DEVINTERFACE_IVSHMEM, 0, &dev_data) == FALSE) {
        SetupDiDestroyDeviceInfoList(device_info);
        return false;
    }

    SetupDiGetDeviceInterfaceDetailA(device_info, &dev_data, NULL, 0, &request_size, NULL);
    if (!request_size) {
        SetupDiDestroyDeviceInfoList(device_info);
        return false;
    }

    inf_data = (PSP_DEVICE_INTERFACE_DETAIL_DATA_A)malloc(request_size);
    ZeroMemory(inf_data, request_size);
    inf_data->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_A);
    if (!SetupDiGetDeviceInterfaceDetailA(device_info, &dev_data, inf_data, request_size, NULL, NULL)) {
        free(inf_data);
        SetupDiDestroyDeviceInfoList(device_info);
        return false;
    }

    handle = CreateFileA(inf_data->DevicePath, 0, 0, NULL, OPEN_EXISTING, 0, 0);
    free(inf_data);
    SetupDiDestroyDeviceInfoList(device_info);
    if (handle == INVALID_HANDLE_VALUE)
        return false;

    /* DeviceIoControl requires a bytes-returned pointer on Windows XP */
    if (!DeviceIoControl(handle, IOCTL_IVSHMEM_REQUEST_SIZE, NULL, 0, &size, sizeof(UINT64), &returned, NULL)) {
        CloseHandle(handle);
        return false;
    }

    /*
     * write-back: reads from write-combined memory are uncached, which makes
     * every poll and framebuffer copy crawl.
     */
    config.cache_mode = IVSHMEM_CACHE_CACHED;
    ZeroMemory(&map, sizeof(struct ivshmem_mmap));
    if (!DeviceIoControl(handle, IOCTL_IVSHMEM_REQUEST_MMAP, &config, sizeof(struct ivshmem_mmap_config), &map, sizeof(struct ivshmem_mmap), &returned, NULL)) {
        CloseHandle(handle);
        return false;
    }

    ivshmem_handle = handle;
    ptr = map.pointer;

    /* the server publishes these at startup and clears them when it exits */
    if (*(volatile uint64_t*)((char*)ptr + SGL_OFFSET_REGISTER_MEMSIZE) == 0 ||
        *(volatile int*)((char*)ptr + SGL_OFFSET_REGISTER_FIFO_SIZE) <= 0) {
        pb_unset();
        ptr = NULL;
        return false;
    }

    base = (PVOID)((uintptr_t)map.pointer + (uintptr_t)SGL_STAGE_OFFSET);
    fifo_size = *(int*)((char*)ptr + SGL_OFFSET_REGISTER_FIFO_SIZE);
    in_capacity = (size_t)fifo_size;
    using_direct_access = direct_access;
    has_client_slot = false;
    write_overflowed = false;

    if (direct_access) {
        in_base = base;
        in_cur = in_base;
    }
    else {
        in_base = VirtualAlloc(NULL, in_capacity, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        in_cur = in_base;
    }
    return in_base != NULL;
}

void pb_unset(void)
{
    DWORD returned = 0;

    if (ivshmem_handle == INVALID_HANDLE_VALUE)
        return;
    DeviceIoControl(ivshmem_handle, IOCTL_IVSHMEM_RELEASE_MMAP, NULL, 0, NULL, 0, &returned, NULL);
    CloseHandle(ivshmem_handle);
    ivshmem_handle = INVALID_HANDLE_VALUE;
}
#endif

void pb_set_net(struct pb_net_hooks hooks, size_t internal_alloc_size)
{
    net_hooks = hooks;
    in_capacity = internal_alloc_size;
    has_client_slot = false;
    write_overflowed = false;
    using_direct_access = false;

#ifndef _WIN32
    in_base = mmap(NULL, internal_alloc_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    in_cur = in_base;
#else
    in_base = VirtualAlloc(NULL, internal_alloc_size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    in_cur = in_base;
#endif
}

void pb_reset()
{
    cur = base;
    in_cur = in_base;
    write_overflowed = false;
}

void pb_push(int c)
{
    if (!pb_can_write(sizeof(c))) {
        pb_note_overflow(sizeof(c));
        return;
    }
    *in_cur++ = c;
}

void pb_push64(int64_t c)
{
    size_t length = CEIL_DIV(sizeof(c), sizeof(*in_cur)) * sizeof(*in_cur);
    if (!pb_can_write(length)) {
        pb_note_overflow(length);
        return;
    }
    memcpy(in_cur, &c, sizeof(c));
    in_cur += CEIL_DIV(sizeof(c), sizeof(*in_cur));
}

void pb_pushf(float c)
{
    if (!pb_can_write(sizeof(c))) {
        pb_note_overflow(sizeof(c));
        return;
    }
    *in_cur++ = *(int*)&c;
}

int pb_read(int s)
{
    if (net_hooks._pb_read)
        return net_hooks._pb_read(s);
    return *(int*)((char*)ptr + pb_resolve_offset((size_t)s));
}

int64_t pb_read64(int s)
{
    if (net_hooks._pb_read64)
        return net_hooks._pb_read64(s);
    return *(int64_t*)((char*)ptr + pb_resolve_offset((size_t)s));
}

void pb_write(int s, int c)
{
    if (net_hooks._pb_write) {
        net_hooks._pb_write(s, c);
        return;
    }
    *(int*)((char*)ptr + pb_resolve_offset((size_t)s)) = c;
}

int pb_global_read(int s)
{
    return *(int*)((char*)ptr + s);
}

int64_t pb_global_read64(int s)
{
    return *(int64_t*)((char*)ptr + s);
}

void pb_global_write(int s, int c)
{
    *(int*)((char*)ptr + s) = c;
}

/*
 *  // equivalent to
 *  int *pdata = (int*)data;
 *  for (int i = 0; i < length / 4; i++)
 *      pb_push(*pdata++);
 */
void pb_memcpy(const void *src, size_t length)
{
    size_t padded = CEIL_DIV(length, sizeof(*in_cur)) * sizeof(*in_cur);
    if (!pb_can_write(padded)) {
        pb_note_overflow(padded);
        return;
    }
    memcpy(in_cur, src, length);
    in_cur += CEIL_DIV(length, 4);
}

void pb_memcpy_unaligned(const void *src, size_t length)
{
    if (!pb_can_write(length)) {
        pb_note_overflow(length);
        return;
    }
    memcpy(in_cur, src, length);
    in_cur = (int*)((char*)in_cur + length);
}

static inline uintptr_t align_to_4(uintptr_t ptr)
{
    return (ptr + 3) & ~3;
}

void pb_realign()
{
    uintptr_t aligned = align_to_4((uintptr_t)in_cur);
    size_t length = aligned - (uintptr_t)in_cur;
    if (!pb_can_write(length)) {
        pb_note_overflow(length);
        return;
    }
    in_cur = (int*)aligned;
}

void *pb_ptr(size_t offs)
{
    if (net_hooks._pb_ptr)
        return net_hooks._pb_ptr(offs);
    return (void*)((char*)ptr + pb_resolve_offset(offs));
}

void *pb_global_ptr(size_t offs)
{
    return (void*)((char*)ptr + offs);
}

void pb_set_client(int client_id)
{
    if (client_id <= 0) {
        has_client_slot = false;
        client_slot_offset = 0;
        return;
    }

    client_slot_offset = SGL_CLIENT_SLOT_OFFSET(client_id);
    has_client_slot = true;
}

void *pb_iptr(size_t offs)
{
    return (void*)((char*)in_base + offs);
}

size_t pb_size()
{
    return (size_t)in_cur - (size_t)in_base;
}

size_t pb_capacity()
{
    return in_capacity;
}

bool pb_overflowed()
{
    return write_overflowed;
}

void pb_copy_to_shared()
{
    if (!using_direct_access && !write_overflowed)
        memcpy(base, in_base, (size_t)in_cur - (size_t)in_base);
}
