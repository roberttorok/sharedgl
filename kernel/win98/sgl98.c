/*
 * SharedGL shared-memory driver for Windows 98 SE.
 *
 * A WDM function driver for the QEMU ivshmem PCI device (1af4:1110), loaded by
 * NTKERN. It answers the same IOCTLs as the XP driver (kernel/winxp/sgldrv.c), so
 * the client's code after opening the device is shared.
 *
 * Differences from XP, all forced by Windows 9x:
 *  - Windows 98 has no device interfaces for applications to find, so the device
 *    is opened by name: \\.\SGL98, a symbolic link in \DosDevices.
 *  - There is no \Device\PhysicalMemory section. It is not needed either: on 9x
 *    the upper 2 GB of the address space are the same in every process. The BAR
 *    is mapped once with MmMapIoSpace, and every client gets that address.
 *  - Ring 3 must be able to reach that mapping. Windows 9x leaves most system
 *    pages user-accessible, but nothing promises that for MmMapIoSpace, so the
 *    driver sets the U/S bit itself, in the shared page tables and in the
 *    calling process's page directory (see SglMakeUserAccessible).
 *
 * Windows 98 has no registry reporting by RtlWriteRegistryValue for services, so
 * the last results are kept in the device and returned by IOCTL_SGL98_STATUS,
 * which sgldiag98.exe prints.
 */
#include <ntddk.h>

#define IOCTL_IVSHMEM_REQUEST_SIZE CTL_CODE(FILE_DEVICE_UNKNOWN, 0x801, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_IVSHMEM_REQUEST_MMAP CTL_CODE(FILE_DEVICE_UNKNOWN, 0x802, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_IVSHMEM_RELEASE_MMAP CTL_CODE(FILE_DEVICE_UNKNOWN, 0x803, METHOD_BUFFERED, FILE_ANY_ACCESS)
/* Windows 98 only: what the driver saw, for diagnostics */
#define IOCTL_SGL98_STATUS         CTL_CODE(FILE_DEVICE_UNKNOWN, 0x820, METHOD_BUFFERED, FILE_ANY_ACCESS)

/* matches struct ivshmem_mmap in src/client/pb.c */
typedef struct _SGL_MMAP {
    USHORT PeerId;
    ULONGLONG Size;
    PVOID Pointer;
    USHORT Vectors;
} SGL_MMAP;

/* matches struct sgl98_status in guest/sgldiag98.c */
typedef struct _SGL98_STATUS {
    ULONG Version;
    ULONG Started;
    ULONG LowerStart;      /* NTSTATUS of the bus driver's start */
    ULONG Descriptors;     /* resources given to the device */
    ULONG MemoryStart;     /* physical address of the shared memory BAR */
    ULONG MemoryLength;
    ULONG Mapped;          /* linear address of the mapping, 0 if none */
    ULONG PagesMadeUser;   /* page table entries that needed the U/S bit */
    ULONG PdesMadeUser;    /* page directory entries that needed it, last caller */
    ULONG LargePages;      /* entries found to be 4 MB pages */
    ULONG Maps;            /* successful REQUEST_MMAP calls */
} SGL98_STATUS;

#define SGL98_VERSION 1

typedef struct _SGL_DEVICE {
    PDEVICE_OBJECT Lower;
    PHYSICAL_ADDRESS MemoryStart;
    ULONG MemoryLength;
    PVOID Mapped;
    BOOLEAN Started;
    BOOLEAN Linked;
    SGL98_STATUS Status;
} SGL_DEVICE;

/*
 * NTKERN is not sure to export the C library's memcpy and memset, which
 * RtlCopyMemory and RtlZeroMemory (and GCC, for structure copies) turn into.
 */
void *memcpy(void *dst, const void *src, size_t n)
{
    unsigned char *d = dst;
    const unsigned char *s = src;
    while (n--)
        *d++ = *s++;
    return dst;
}

void *memset(void *dst, int c, size_t n)
{
    unsigned char *d = dst;
    while (n--)
        *d++ = (unsigned char)c;
    return dst;
}

static const WCHAR device_name[] = L"\\Device\\SGL98";
static const WCHAR link_name[] = L"\\DosDevices\\SGL98";

static NTSTATUS SglComplete(PIRP Irp, NTSTATUS Status, ULONG_PTR Information)
{
    Irp->IoStatus.Status = Status;
    Irp->IoStatus.Information = Information;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return Status;
}

static NTSTATUS NTAPI SglPassDown(PDEVICE_OBJECT Device, PIRP Irp)
{
    SGL_DEVICE *ext = Device->DeviceExtension;
    IoSkipCurrentIrpStackLocation(Irp);
    return IoCallDriver(ext->Lower, Irp);
}

static NTSTATUS NTAPI SglSignal(PDEVICE_OBJECT Device, PIRP Irp, PVOID Context)
{
    UNREFERENCED_PARAMETER(Device);
    UNREFERENCED_PARAMETER(Irp);
    KeSetEvent((PKEVENT)Context, IO_NO_INCREMENT, FALSE);
    return STATUS_MORE_PROCESSING_REQUIRED;
}

static NTSTATUS SglForwardAndWait(PDEVICE_OBJECT Device, PIRP Irp)
{
    SGL_DEVICE *ext = Device->DeviceExtension;
    KEVENT event;
    NTSTATUS status;

    KeInitializeEvent(&event, NotificationEvent, FALSE);
    IoCopyCurrentIrpStackLocationToNext(Irp);
    IoSetCompletionRoutine(Irp, SglSignal, &event, TRUE, TRUE, TRUE);
    status = IoCallDriver(ext->Lower, Irp);
    if (status == STATUS_PENDING) {
        KeWaitForSingleObject(&event, Executive, KernelMode, FALSE, NULL);
        status = Irp->IoStatus.Status;
    }
    return status;
}

/* ivshmem has the 256-byte register BAR and the shared memory BAR; take the latter */
static NTSTATUS SglFindMemory(SGL_DEVICE *Ext, PCM_RESOURCE_LIST Resources)
{
    ULONG i;

    if (Resources == NULL || Resources->Count == 0) {
        Ext->Status.Descriptors = 0xffffffff;
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }

    Ext->Status.Descriptors = Resources->List[0].PartialResourceList.Count;

    /* a pointer, not an index: PartialDescriptors is declared with one element */
    PCM_PARTIAL_RESOURCE_DESCRIPTOR d = (PVOID)Resources->List[0].PartialResourceList.PartialDescriptors;
    for (i = 0; i < Resources->List[0].PartialResourceList.Count; i++, d++) {
        if (d->Type == CmResourceTypeMemory && d->u.Memory.Length > Ext->MemoryLength) {
            Ext->MemoryStart = d->u.Memory.Start;
            Ext->MemoryLength = d->u.Memory.Length;
        }
    }

    Ext->Status.MemoryStart = Ext->MemoryStart.LowPart;
    Ext->Status.MemoryLength = Ext->MemoryLength;
    if (Ext->MemoryLength <= 0x1000)
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    return STATUS_SUCCESS;
}

/* ---- page tables ---- */

#define PTE_PRESENT 0x001
#define PTE_USER    0x004
#define PDE_LARGE   0x080

static ULONG SglReadCr3(void)
{
    ULONG value;
    __asm__ __volatile__("mov %%cr3, %0" : "=r"(value));
    return value;
}

static ULONG SglReadCr4(void)
{
    ULONG value;
    __asm__ __volatile__("mov %%cr4, %0" : "=r"(value));
    return value;
}

static void SglFlushTlb(void)
{
    ULONG cr3;
    __asm__ __volatile__("mov %%cr3, %0; mov %0, %%cr3" : "=r"(cr3) :: "memory");
}

static void SglInvalidate(PVOID Address)
{
    __asm__ __volatile__("invlpg (%0)" :: "r"(Address) : "memory");
}

/*
 * A window onto one physical page. MmMapIoSpace takes any physical address. Page
 * tables are ordinary RAM, so map them cached like every other mapping of them.
 */
static volatile ULONG *SglMapPage(ULONG Physical)
{
    PHYSICAL_ADDRESS address;
    address.QuadPart = Physical & ~0xfffu;
    return MmMapIoSpace(address, PAGE_SIZE, MmCached);
}

static void SglUnmapPage(volatile ULONG *Page)
{
    MmUnmapIoSpace((PVOID)Page, PAGE_SIZE);
}

/*
 * Set U/S on every page of [Base, Base+Length): in the page table entries, which
 * all processes share for the system arena, and in the current page directory.
 * Windows 98 does not use PAE. Called at PASSIVE_LEVEL in the caller's context.
 */
static void SglMakeUserAccessible(SGL_DEVICE *Ext, PVOID Base, ULONG Length)
{
    ULONG va = (ULONG)Base & ~0xfffu;
    ULONG end = (ULONG)Base + Length;
    volatile ULONG *directory;
    ULONG pdes = 0, ptes = 0, large = 0;

    if (SglReadCr4() & 0x20)
        return; /* PAE: never on Windows 98 */

    directory = SglMapPage(SglReadCr3());
    if (directory == NULL)
        return;

    while (va < end) {
        ULONG pde_index = va >> 22;
        ULONG pde = directory[pde_index];
        ULONG next = (va & 0xffc00000u) + 0x400000u;

        if (!(pde & PTE_PRESENT)) {
            va = next;
            continue;
        }
        if (!(pde & PTE_USER)) {
            directory[pde_index] = pde | PTE_USER;
            pdes++;
        }
        if (pde & PDE_LARGE) {
            large++;
        }
        else {
            volatile ULONG *table = SglMapPage(pde & 0xfffff000u);
            if (table != NULL) {
                for (; va < next && va < end; va += PAGE_SIZE) {
                    ULONG index = (va >> 12) & 0x3ff;
                    ULONG pte = table[index];
                    if ((pte & PTE_PRESENT) && !(pte & PTE_USER)) {
                        table[index] = pte | PTE_USER;
                        SglInvalidate((PVOID)va);
                        ptes++;
                    }
                }
                SglUnmapPage(table);
            }
        }
        va = next;
    }

    SglUnmapPage(directory);
    if (pdes || large)
        SglFlushTlb();

    Ext->Status.PagesMadeUser += ptes;
    Ext->Status.PdesMadeUser = pdes;
    Ext->Status.LargePages = large;
}

/* ---- PnP ---- */

static void SglStop(SGL_DEVICE *Ext)
{
    if (Ext->Mapped != NULL)
        MmUnmapIoSpace(Ext->Mapped, Ext->MemoryLength);
    Ext->Mapped = NULL;
    Ext->Started = FALSE;
    Ext->MemoryLength = 0;
    Ext->Status.Started = 0;
    Ext->Status.Mapped = 0;
}

static NTSTATUS NTAPI SglPnp(PDEVICE_OBJECT Device, PIRP Irp)
{
    SGL_DEVICE *ext = Device->DeviceExtension;
    PIO_STACK_LOCATION stack = IoGetCurrentIrpStackLocation(Irp);
    NTSTATUS status;

    switch (stack->MinorFunction) {
    case IRP_MN_START_DEVICE:
        status = SglForwardAndWait(Device, Irp);
        ext->Status.LowerStart = status;
        if (NT_SUCCESS(status))
            status = SglFindMemory(ext, stack->Parameters.StartDevice.AllocatedResourcesTranslated);
        if (NT_SUCCESS(status)) {
            /* write-back: QEMU's shared memory is ordinary host RAM, coherent with the host */
            ext->Mapped = MmMapIoSpace(ext->MemoryStart, ext->MemoryLength, MmCached);
            if (ext->Mapped == NULL)
                status = STATUS_INSUFFICIENT_RESOURCES;
        }
        if (NT_SUCCESS(status)) {
            ext->Started = TRUE;
            ext->Status.Started = 1;
            ext->Status.Mapped = (ULONG)ext->Mapped;
        }
        return SglComplete(Irp, status, 0);

    case IRP_MN_QUERY_STOP_DEVICE:
    case IRP_MN_CANCEL_STOP_DEVICE:
    case IRP_MN_QUERY_REMOVE_DEVICE:
    case IRP_MN_CANCEL_REMOVE_DEVICE:
        Irp->IoStatus.Status = STATUS_SUCCESS;
        return SglPassDown(Device, Irp);

    case IRP_MN_STOP_DEVICE:
    case IRP_MN_SURPRISE_REMOVAL:
        SglStop(ext);
        Irp->IoStatus.Status = STATUS_SUCCESS;
        return SglPassDown(Device, Irp);

    case IRP_MN_REMOVE_DEVICE: {
        PDEVICE_OBJECT lower = ext->Lower;
        UNICODE_STRING link;
        SglStop(ext);
        Irp->IoStatus.Status = STATUS_SUCCESS;
        IoSkipCurrentIrpStackLocation(Irp);
        status = IoCallDriver(lower, Irp);
        if (ext->Linked) {
            RtlInitUnicodeString(&link, link_name);
            IoDeleteSymbolicLink(&link);
        }
        IoDetachDevice(lower);
        IoDeleteDevice(Device);
        return status;
    }

    default:
        return SglPassDown(Device, Irp);
    }
}

static NTSTATUS NTAPI SglPower(PDEVICE_OBJECT Device, PIRP Irp)
{
    SGL_DEVICE *ext = Device->DeviceExtension;
    PoStartNextPowerIrp(Irp);
    IoSkipCurrentIrpStackLocation(Irp);
    return PoCallDriver(ext->Lower, Irp);
}

static NTSTATUS NTAPI SglDeviceControl(PDEVICE_OBJECT Device, PIRP Irp)
{
    SGL_DEVICE *ext = Device->DeviceExtension;
    PIO_STACK_LOCATION stack = IoGetCurrentIrpStackLocation(Irp);
    ULONG out_length = stack->Parameters.DeviceIoControl.OutputBufferLength;
    PVOID buffer = Irp->AssociatedIrp.SystemBuffer;

    switch (stack->Parameters.DeviceIoControl.IoControlCode) {
    case IOCTL_SGL98_STATUS:
        if (out_length < sizeof(SGL98_STATUS))
            return SglComplete(Irp, STATUS_INVALID_BUFFER_SIZE, 0);
        ext->Status.Version = SGL98_VERSION;
        RtlCopyMemory(buffer, &ext->Status, sizeof(SGL98_STATUS));
        return SglComplete(Irp, STATUS_SUCCESS, sizeof(SGL98_STATUS));

    case IOCTL_IVSHMEM_REQUEST_SIZE:
        if (!ext->Started)
            return SglComplete(Irp, STATUS_DEVICE_NOT_READY, 0);
        if (out_length != sizeof(ULONGLONG))
            return SglComplete(Irp, STATUS_INVALID_BUFFER_SIZE, 0);
        *(ULONGLONG*)buffer = ext->MemoryLength;
        return SglComplete(Irp, STATUS_SUCCESS, sizeof(ULONGLONG));

    case IOCTL_IVSHMEM_REQUEST_MMAP: {
        SGL_MMAP result;
        if (!ext->Started)
            return SglComplete(Irp, STATUS_DEVICE_NOT_READY, 0);
        /* the cache mode byte is accepted and ignored: the one mapping is write-back */
        if (out_length != sizeof(SGL_MMAP))
            return SglComplete(Irp, STATUS_INVALID_BUFFER_SIZE, 0);
        SglMakeUserAccessible(ext, ext->Mapped, ext->MemoryLength);
        RtlZeroMemory(&result, sizeof(result));
        result.Size = ext->MemoryLength;
        result.Pointer = ext->Mapped;
        RtlCopyMemory(buffer, &result, sizeof(result));
        ext->Status.Maps++;
        return SglComplete(Irp, STATUS_SUCCESS, sizeof(result));
    }

    case IOCTL_IVSHMEM_RELEASE_MMAP:
        /* shared by every process; it goes when the device stops */
        return SglComplete(Irp, STATUS_SUCCESS, 0);

    default:
        return SglComplete(Irp, STATUS_INVALID_DEVICE_REQUEST, 0);
    }
}

static NTSTATUS NTAPI SglCreateClose(PDEVICE_OBJECT Device, PIRP Irp)
{
    UNREFERENCED_PARAMETER(Device);
    return SglComplete(Irp, STATUS_SUCCESS, 0);
}

static NTSTATUS NTAPI SglAddDevice(PDRIVER_OBJECT Driver, PDEVICE_OBJECT Pdo)
{
    PDEVICE_OBJECT device;
    SGL_DEVICE *ext;
    UNICODE_STRING name, link;
    NTSTATUS status;

    RtlInitUnicodeString(&name, device_name);
    status = IoCreateDevice(Driver, sizeof(SGL_DEVICE), &name, FILE_DEVICE_UNKNOWN, 0, FALSE, &device);
    if (!NT_SUCCESS(status))
        return status;

    ext = device->DeviceExtension;
    RtlZeroMemory(ext, sizeof(*ext));

    RtlInitUnicodeString(&link, link_name);
    ext->Linked = NT_SUCCESS(IoCreateSymbolicLink(&link, &name));

    ext->Lower = IoAttachDeviceToDeviceStack(device, Pdo);
    if (ext->Lower == NULL) {
        if (ext->Linked)
            IoDeleteSymbolicLink(&link);
        IoDeleteDevice(device);
        return STATUS_NO_SUCH_DEVICE;
    }

    device->Flags |= DO_BUFFERED_IO | DO_POWER_PAGABLE;
    device->Flags &= ~DO_DEVICE_INITIALIZING;
    return STATUS_SUCCESS;
}

static VOID NTAPI SglUnload(PDRIVER_OBJECT Driver)
{
    UNREFERENCED_PARAMETER(Driver);
}

NTSTATUS NTAPI DriverEntry(PDRIVER_OBJECT Driver, PUNICODE_STRING Registry)
{
    ULONG i;

    UNREFERENCED_PARAMETER(Registry);

    for (i = 0; i <= IRP_MJ_MAXIMUM_FUNCTION; i++)
        Driver->MajorFunction[i] = SglPassDown;

    Driver->MajorFunction[IRP_MJ_CREATE] = SglCreateClose;
    Driver->MajorFunction[IRP_MJ_CLOSE] = SglCreateClose;
    Driver->MajorFunction[IRP_MJ_CLEANUP] = SglCreateClose;
    Driver->MajorFunction[IRP_MJ_DEVICE_CONTROL] = SglDeviceControl;
    Driver->MajorFunction[IRP_MJ_PNP] = SglPnp;
    Driver->MajorFunction[IRP_MJ_POWER] = SglPower;
    Driver->DriverExtension->AddDevice = SglAddDevice;
    Driver->DriverUnload = SglUnload;
    return STATUS_SUCCESS;
}
