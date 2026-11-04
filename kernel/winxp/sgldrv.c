/*
 * SharedGL shared-memory driver for 32-bit Windows XP.
 *
 * The upstream driver needs KMDF 1.15, which does not exist on XP, so this is a
 * plain WDM function driver for the QEMU ivshmem PCI device. It offers the same
 * device interface GUID and IOCTLs, so the client does not know the difference.
 *
 * Only the shared memory BAR is used. It is mapped into each client through
 * \Device\PhysicalMemory rather than an MDL: on XP an MDL cannot describe
 * 64 MiB or more, and this way the BAR costs no kernel address space.
 */
#include <ntddk.h>
#include <initguid.h>

DEFINE_GUID(GUID_DEVINTERFACE_IVSHMEM,
    0xdf576976, 0x569d, 0x4672, 0x95, 0xa0, 0xf5, 0x7e, 0x4e, 0xa0, 0xb2, 0x10);

#define IOCTL_IVSHMEM_REQUEST_SIZE CTL_CODE(FILE_DEVICE_UNKNOWN, 0x801, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_IVSHMEM_REQUEST_MMAP CTL_CODE(FILE_DEVICE_UNKNOWN, 0x802, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_IVSHMEM_RELEASE_MMAP CTL_CODE(FILE_DEVICE_UNKNOWN, 0x803, METHOD_BUFFERED, FILE_ANY_ACCESS)
/* SharedGL's own: map the QXL display card's memory write-back, see SglCacheQxl() */
#define IOCTL_SGL_CACHE_QXL        CTL_CODE(FILE_DEVICE_UNKNOWN, 0x810, METHOD_BUFFERED, FILE_ANY_ACCESS)

#define IVSHMEM_CACHE_NONCACHED     0
#define IVSHMEM_CACHE_CACHED        1
#define IVSHMEM_CACHE_WRITECOMBINED 2

/* matches struct ivshmem_mmap in src/client/pb.c */
typedef struct _SGL_MMAP {
    USHORT PeerId;
    ULONGLONG Size;
    PVOID Pointer;
    USHORT Vectors;
} SGL_MMAP;

typedef struct _SGL_DEVICE {
    PDEVICE_OBJECT Lower;
    UNICODE_STRING InterfaceName;
    PHYSICAL_ADDRESS MemoryStart;
    ULONG MemoryLength;
    BOOLEAN Started;
} SGL_DEVICE;

/* DbgPrint needs a kernel debugger; this is readable with reg query */
static void SglReport(PCWSTR Name, ULONG Value)
{
    RtlWriteRegistryValue(RTL_REGISTRY_SERVICES, L"sgldrv", Name, REG_DWORD, &Value, sizeof(Value));
}

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
        SglReport(L"LastDescriptors", 0xffffffff);
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }

    SglReport(L"LastDescriptors", Resources->List[0].PartialResourceList.Count);
    RtlWriteRegistryValue(RTL_REGISTRY_SERVICES, L"sgldrv", L"LastResources", REG_BINARY,
        Resources->List[0].PartialResourceList.PartialDescriptors,
        Resources->List[0].PartialResourceList.Count * sizeof(CM_PARTIAL_RESOURCE_DESCRIPTOR));

    /*
     * Walk with a pointer: PartialDescriptors is declared with one element, so
     * GCC would otherwise assume that an index above 0 never happens and drop
     * the rest of the loop.
     */
    PCM_PARTIAL_RESOURCE_DESCRIPTOR d = (PVOID)Resources->List[0].PartialResourceList.PartialDescriptors;
    for (i = 0; i < Resources->List[0].PartialResourceList.Count; i++, d++) {
        if (d->Type == CmResourceTypeMemory && d->u.Memory.Length > Ext->MemoryLength) {
            Ext->MemoryStart = d->u.Memory.Start;
            Ext->MemoryLength = d->u.Memory.Length;
        }
    }

    if (Ext->MemoryLength <= 0x1000)
        return STATUS_DEVICE_CONFIGURATION_ERROR;

    SglReport(L"LastMemoryStart", Ext->MemoryStart.LowPart);
    SglReport(L"LastMemoryLength", Ext->MemoryLength);
    return STATUS_SUCCESS;
}

static NTSTATUS NTAPI SglPnp(PDEVICE_OBJECT Device, PIRP Irp)
{
    SGL_DEVICE *ext = Device->DeviceExtension;
    PIO_STACK_LOCATION stack = IoGetCurrentIrpStackLocation(Irp);
    NTSTATUS status;

    switch (stack->MinorFunction) {
    case IRP_MN_START_DEVICE:
        status = SglForwardAndWait(Device, Irp);
        SglReport(L"LastLowerStart", status);
        if (NT_SUCCESS(status))
            status = SglFindMemory(ext, stack->Parameters.StartDevice.AllocatedResourcesTranslated);
        if (NT_SUCCESS(status))
            status = IoSetDeviceInterfaceState(&ext->InterfaceName, TRUE);
        SglReport(L"LastStart", status);
        if (NT_SUCCESS(status))
            ext->Started = TRUE;
        return SglComplete(Irp, status, 0);

    case IRP_MN_QUERY_STOP_DEVICE:
    case IRP_MN_CANCEL_STOP_DEVICE:
    case IRP_MN_QUERY_REMOVE_DEVICE:
    case IRP_MN_CANCEL_REMOVE_DEVICE:
        Irp->IoStatus.Status = STATUS_SUCCESS;
        return SglPassDown(Device, Irp);

    case IRP_MN_STOP_DEVICE:
    case IRP_MN_SURPRISE_REMOVAL:
        if (ext->Started)
            IoSetDeviceInterfaceState(&ext->InterfaceName, FALSE);
        ext->Started = FALSE;
        ext->MemoryLength = 0;
        Irp->IoStatus.Status = STATUS_SUCCESS;
        return SglPassDown(Device, Irp);

    case IRP_MN_REMOVE_DEVICE: {
        PDEVICE_OBJECT lower = ext->Lower;
        if (ext->Started)
            IoSetDeviceInterfaceState(&ext->InterfaceName, FALSE);
        ext->Started = FALSE;
        Irp->IoStatus.Status = STATUS_SUCCESS;
        IoSkipCurrentIrpStackLocation(Irp);
        status = IoCallDriver(lower, Irp);
        RtlFreeUnicodeString(&ext->InterfaceName);
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

static NTSTATUS SglUnmap(PFILE_OBJECT File)
{
    PVOID base = File->FsContext;
    if (base == NULL)
        return STATUS_INVALID_DEVICE_REQUEST;
    File->FsContext = NULL;
    return ZwUnmapViewOfSection(NtCurrentProcess(), base);
}

/* Runs in the calling process, because this is the top of the device stack. */
static NTSTATUS SglMap(SGL_DEVICE *Ext, PFILE_OBJECT File, UCHAR CacheMode, SGL_MMAP *Out)
{
    static const WCHAR section_name[] = L"\\Device\\PhysicalMemory";
    UNICODE_STRING name;
    OBJECT_ATTRIBUTES attributes;
    HANDLE section;
    PVOID base = NULL;
    SIZE_T view_size = Ext->MemoryLength;
    LARGE_INTEGER offset = Ext->MemoryStart;
    ULONG protect = PAGE_READWRITE;
    NTSTATUS status;

    if (File->FsContext != NULL)
        return STATUS_DEVICE_ALREADY_ATTACHED;

    switch (CacheMode) {
    case IVSHMEM_CACHE_NONCACHED:
        protect |= PAGE_NOCACHE;
        break;
    case IVSHMEM_CACHE_CACHED:
        break;
    case IVSHMEM_CACHE_WRITECOMBINED:
        protect |= PAGE_WRITECOMBINE;
        break;
    default:
        return STATUS_INVALID_PARAMETER;
    }

    RtlInitUnicodeString(&name, section_name);
    InitializeObjectAttributes(&attributes, &name, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
    status = ZwOpenSection(&section, SECTION_ALL_ACCESS, &attributes);
    if (!NT_SUCCESS(status))
        return status;

    status = ZwMapViewOfSection(section, NtCurrentProcess(), &base, 0, 0, &offset, &view_size, ViewUnmap, 0, protect);
    ZwClose(section);
    if (!NT_SUCCESS(status)) {
        SglReport(L"LastMapFailure", status);
        return status;
    }

    File->FsContext = base;
    /* the view starts on an allocation boundary at or below the BAR */
    Out->PeerId = 0;
    Out->Size = Ext->MemoryLength;
    Out->Pointer = (PUCHAR)base + (Ext->MemoryStart.QuadPart - offset.QuadPart);
    Out->Vectors = 0;
    return STATUS_SUCCESS;
}

/*
 * The QXL display driver maps the card's memory uncached (PCD+PWT in its page table
 * entries). Under KVM on an AMD host that is honoured, so every store of a frame into
 * the card is a separate trip to memory: a 1024x768 GDI blit costs about 24 ms of
 * guest CPU. In QEMU that memory is ordinary RAM, coherent with the host, so
 * write-back is safe, and it brings the blit down to a plain memcpy.
 *
 * This finds the card on PCI bus 0, takes each of its memory BARs up to the next BAR
 * of any device on the bus (sizing a BAR would mean rewriting it while the display
 * driver uses it), and clears PCD/PWT in every kernel mapping that points into one.
 * Only system space is changed, which is where the QXL drivers map the card.
 * Calling it again is harmless and catches mappings made since, such as after a
 * mode change.
 */
#define SGL_MAX_RANGES 8

typedef struct _SGL_CACHE_RESULT {
    ULONG Ranges;      /* QXL memory BARs found */
    ULONG Patched;     /* page table entries changed */
    ULONG Already;     /* entries into the card that were already cached */
    ULONG Pae;
} SGL_CACHE_RESULT;

static ULONG SglQxlRanges(ULONGLONG *Start, ULONGLONG *End)
{
    ULONGLONG bases[32 * 6];
    ULONG nbases = 0, nranges = 0, slot, i, j;

    for (slot = 0; slot < 32; slot++) {
        ULONG cfg[10];
        if (HalGetBusDataByOffset(PCIConfiguration, 0, slot, cfg, 0, sizeof(cfg)) != sizeof(cfg))
            continue;
        if ((cfg[0] & 0xffff) == 0xffff || (cfg[3] >> 16 & 0x7f) != 0)
            continue; /* empty slot, or not a plain device (a bridge has 2 BARs) */
        BOOLEAN qxl = cfg[0] == 0x01001b36;
        for (i = 0; i < 6; i++) {
            ULONG bar = cfg[4 + i];
            ULONGLONG base;
            if (bar & 1)
                continue; /* I/O */
            base = bar & ~0xfu;
            if ((bar & 6) == 4 && i < 5)
                base |= (ULONGLONG)cfg[4 + ++i] << 32;
            if (base == 0)
                continue;
            bases[nbases++] = base;
            if (qxl && nranges < SGL_MAX_RANGES) {
                Start[nranges] = base;
                End[nranges++] = 0;
            }
        }
    }

    for (i = 0; i < nranges; i++) {
        ULONGLONG end = Start[i] + 0x20000000; /* no QXL BAR is bigger */
        for (j = 0; j < nbases; j++)
            if (bases[j] > Start[i] && bases[j] < end)
                end = bases[j];
        if (end > 0xfec00000 && Start[i] < 0xfec00000)
            end = 0xfec00000; /* never the APICs */
        End[i] = end;
    }
    return nranges;
}

static BOOLEAN SglInRanges(ULONGLONG Address, const ULONGLONG *Start, const ULONGLONG *End, ULONG Count)
{
    ULONG i;
    for (i = 0; i < Count; i++)
        if (Address >= Start[i] && Address < End[i])
            return TRUE;
    return FALSE;
}

static ULONG SglReadCr4(void)
{
    ULONG value;
    __asm__ __volatile__("mov %%cr4, %0" : "=r"(value));
    return value;
}

/* Toggling CR4.PGE flushes the whole TLB, global entries included. */
static void SglFlushTlb(void)
{
    ULONG cr4;
    __asm__ __volatile__("pushf; cli; mov %%cr4, %0; xorl $0x80, %0; mov %0, %%cr4; xorl $0x80, %0; mov %0, %%cr4; popf"
                         : "=&r"(cr4) :: "memory");
}

#define SGL_PCD_PWT 0x18

static void SglCacheQxl(SGL_CACHE_RESULT *Result)
{
    ULONGLONG start[SGL_MAX_RANGES], end[SGL_MAX_RANGES];
    ULONG va, cpu;
    KAFFINITY active;
    BOOLEAN pae = (SglReadCr4() & 0x20) != 0;

    RtlZeroMemory(Result, sizeof(*Result));
    Result->Pae = pae;
    Result->Ranges = SglQxlRanges(start, end);
    if (Result->Ranges == 0)
        return;

    /* The page tables are mapped at 0xC0000000 (PTEs) and just above (PDEs). */
    for (va = 0x80000000u; va >= 0x80000000u; ) {
        ULONG step = pae ? 0x200000 : 0x400000;
        if (pae) {
            volatile ULONGLONG *pde = (ULONGLONG*)(0xC0600000u + (va >> 21) * 8);
            ULONGLONG e = *pde;
            if (e & 1) {
                if (e & 0x80) {
                    if (SglInRanges(e & 0xfffffffe00000ull, start, end, Result->Ranges)) {
                        if (e & SGL_PCD_PWT) { *pde = e & ~(ULONGLONG)SGL_PCD_PWT; Result->Patched++; }
                        else Result->Already++;
                    }
                }
                else {
                    volatile ULONGLONG *pte = (ULONGLONG*)(0xC0000000u + (va >> 12) * 8);
                    ULONG k;
                    for (k = 0; k < 512; k++) {
                        ULONGLONG t = pte[k];
                        if ((t & 1) && SglInRanges(t & 0xffffffffff000ull, start, end, Result->Ranges)) {
                            if (t & SGL_PCD_PWT) { pte[k] = t & ~(ULONGLONG)SGL_PCD_PWT; Result->Patched++; }
                            else Result->Already++;
                        }
                    }
                }
            }
        }
        else {
            volatile ULONG *pde = (ULONG*)(0xC0300000u + (va >> 22) * 4);
            ULONG e = *pde;
            if (e & 1) {
                if (e & 0x80) {
                    if (SglInRanges(e & 0xffc00000u, start, end, Result->Ranges)) {
                        if (e & SGL_PCD_PWT) { *pde = e & ~SGL_PCD_PWT; Result->Patched++; }
                        else Result->Already++;
                    }
                }
                else {
                    volatile ULONG *pte = (ULONG*)(0xC0000000u + (va >> 12) * 4);
                    ULONG k;
                    for (k = 0; k < 1024; k++) {
                        ULONG t = pte[k];
                        if ((t & 1) && SglInRanges(t & 0xfffff000u, start, end, Result->Ranges)) {
                            if (t & SGL_PCD_PWT) { pte[k] = t & ~SGL_PCD_PWT; Result->Patched++; }
                            else Result->Already++;
                        }
                    }
                }
            }
        }
        va += step;
    }

    if (Result->Patched == 0)
        return;
    /* The memory was uncached, so no cache holds stale lines of it: a TLB flush is enough. */
    active = KeQueryActiveProcessors();
    for (cpu = 0; cpu < sizeof(KAFFINITY) * 8; cpu++) {
        if (!(active & ((KAFFINITY)1 << cpu)))
            continue;
        KeSetSystemAffinityThread((KAFFINITY)1 << cpu);
        SglFlushTlb();
    }
    KeRevertToUserAffinityThread();
}

static NTSTATUS NTAPI SglDeviceControl(PDEVICE_OBJECT Device, PIRP Irp)
{
    SGL_DEVICE *ext = Device->DeviceExtension;
    PIO_STACK_LOCATION stack = IoGetCurrentIrpStackLocation(Irp);
    ULONG in_length = stack->Parameters.DeviceIoControl.InputBufferLength;
    ULONG out_length = stack->Parameters.DeviceIoControl.OutputBufferLength;
    PVOID buffer = Irp->AssociatedIrp.SystemBuffer;

    if (!ext->Started)
        return SglComplete(Irp, STATUS_DEVICE_NOT_READY, 0);

    switch (stack->Parameters.DeviceIoControl.IoControlCode) {
    case IOCTL_IVSHMEM_REQUEST_SIZE:
        if (out_length != sizeof(ULONGLONG))
            return SglComplete(Irp, STATUS_INVALID_BUFFER_SIZE, 0);
        *(ULONGLONG*)buffer = ext->MemoryLength;
        return SglComplete(Irp, STATUS_SUCCESS, sizeof(ULONGLONG));

    case IOCTL_IVSHMEM_REQUEST_MMAP: {
        SGL_MMAP result;
        NTSTATUS status;
        if (in_length != sizeof(UCHAR) || out_length != sizeof(SGL_MMAP))
            return SglComplete(Irp, STATUS_INVALID_BUFFER_SIZE, 0);
        RtlZeroMemory(&result, sizeof(result));
        status = SglMap(ext, stack->FileObject, *(UCHAR*)buffer, &result);
        if (!NT_SUCCESS(status))
            return SglComplete(Irp, status, 0);
        RtlCopyMemory(buffer, &result, sizeof(result));
        return SglComplete(Irp, STATUS_SUCCESS, sizeof(result));
    }

    case IOCTL_IVSHMEM_RELEASE_MMAP:
        return SglComplete(Irp, SglUnmap(stack->FileObject), 0);

    case IOCTL_SGL_CACHE_QXL: {
        SGL_CACHE_RESULT result;
        if (out_length < sizeof(result))
            return SglComplete(Irp, STATUS_INVALID_BUFFER_SIZE, 0);
        SglCacheQxl(&result);
        SglReport(L"QxlRanges", result.Ranges);
        SglReport(L"QxlPatched", result.Patched);
        SglReport(L"QxlAlready", result.Already);
        RtlCopyMemory(buffer, &result, sizeof(result));
        return SglComplete(Irp, STATUS_SUCCESS, sizeof(result));
    }

    default:
        return SglComplete(Irp, STATUS_INVALID_DEVICE_REQUEST, 0);
    }
}

static NTSTATUS NTAPI SglCreateClose(PDEVICE_OBJECT Device, PIRP Irp)
{
    UNREFERENCED_PARAMETER(Device);
    return SglComplete(Irp, STATUS_SUCCESS, 0);
}

/* Sent in the context of the process closing its last handle, even when it is exiting. */
static NTSTATUS NTAPI SglCleanup(PDEVICE_OBJECT Device, PIRP Irp)
{
    UNREFERENCED_PARAMETER(Device);
    SglUnmap(IoGetCurrentIrpStackLocation(Irp)->FileObject);
    return SglComplete(Irp, STATUS_SUCCESS, 0);
}

static NTSTATUS NTAPI SglAddDevice(PDRIVER_OBJECT Driver, PDEVICE_OBJECT Pdo)
{
    PDEVICE_OBJECT device;
    SGL_DEVICE *ext;
    NTSTATUS status;

    status = IoCreateDevice(Driver, sizeof(SGL_DEVICE), NULL, FILE_DEVICE_UNKNOWN, FILE_DEVICE_SECURE_OPEN, FALSE, &device);
    if (!NT_SUCCESS(status))
        return status;

    ext = device->DeviceExtension;
    RtlZeroMemory(ext, sizeof(*ext));

    status = IoRegisterDeviceInterface(Pdo, &GUID_DEVINTERFACE_IVSHMEM, NULL, &ext->InterfaceName);
    if (!NT_SUCCESS(status)) {
        IoDeleteDevice(device);
        return status;
    }

    ext->Lower = IoAttachDeviceToDeviceStack(device, Pdo);
    if (ext->Lower == NULL) {
        RtlFreeUnicodeString(&ext->InterfaceName);
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
    Driver->MajorFunction[IRP_MJ_CLEANUP] = SglCleanup;
    Driver->MajorFunction[IRP_MJ_DEVICE_CONTROL] = SglDeviceControl;
    Driver->MajorFunction[IRP_MJ_PNP] = SglPnp;
    Driver->MajorFunction[IRP_MJ_POWER] = SglPower;
    Driver->DriverExtension->AddDevice = SglAddDevice;
    Driver->DriverUnload = SglUnload;
    return STATUS_SUCCESS;
}
