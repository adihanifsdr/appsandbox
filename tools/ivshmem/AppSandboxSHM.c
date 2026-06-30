/*
 * AppSandboxSHM.c — AppSandbox ivshmem BAR-mapping driver (KMDF). Our own code, no third party.
 *
 * Binds QEMU's ivshmem-plain PCI device (VEN_1AF4&DEV_1110), finds the large prefetchable
 * memory BAR (BAR2 = the shared window), and on IOCTL maps it into the caller's user address
 * space. The macOS host mmaps the same backing file -> zero-copy host<->guest shared memory.
 *
 * Build: WDK + VS2022 (KMDF). Target ARM64. Test-sign (the dev VM runs with testsigning on).
 *
 * NOTE (verify on first build/run): the user-mode mapping is created by
 * MmMapLockedPagesSpecifyCache(UserMode), which maps into the *current* process. The default
 * WDF parallel queue dispatches EvtIoDeviceControl in the requesting thread's context, so the
 * mapping lands in the caller's process. The mapping is torn down in EvtFileCleanup, which also
 * runs in the closing process's context.
 */
#include <ntddk.h>
#include <wdf.h>
#include "AppSandboxSHM.h"

typedef struct _DEVICE_CONTEXT {
    PHYSICAL_ADDRESS bar2Pa;     /* physical base of the shared BAR        */
    SIZE_T           bar2Len;    /* size in bytes                          */
    PVOID            bar2SysVa;  /* optional kernel mapping (write-combined) */
} DEVICE_CONTEXT, *PDEVICE_CONTEXT;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(DEVICE_CONTEXT, DeviceGetContext)

/* Per-handle state so each open's user mapping is cleaned up on close. */
typedef struct _FILE_CONTEXT {
    PMDL  mdl;
    PVOID userVa;
} FILE_CONTEXT, *PFILE_CONTEXT;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(FILE_CONTEXT, FileGetContext)

DRIVER_INITIALIZE              DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD      AsbEvtDeviceAdd;
EVT_WDF_DEVICE_PREPARE_HARDWARE AsbEvtPrepareHardware;
EVT_WDF_DEVICE_RELEASE_HARDWARE AsbEvtReleaseHardware;
EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL AsbEvtIoDeviceControl;
EVT_WDF_FILE_CLEANUP           AsbEvtFileCleanup;

NTSTATUS DriverEntry(PDRIVER_OBJECT drv, PUNICODE_STRING reg)
{
    WDF_DRIVER_CONFIG cfg;
    WDF_DRIVER_CONFIG_INIT(&cfg, AsbEvtDeviceAdd);
    return WdfDriverCreate(drv, reg, WDF_NO_OBJECT_ATTRIBUTES, &cfg, WDF_NO_HANDLE);
}

NTSTATUS AsbEvtDeviceAdd(WDFDRIVER drv, PWDFDEVICE_INIT init)
{
    NTSTATUS st;
    WDF_PNPPOWER_EVENT_CALLBACKS pnp;
    WDF_FILEOBJECT_CONFIG foCfg;
    WDF_OBJECT_ATTRIBUTES foAttr, devAttr;
    WDF_IO_QUEUE_CONFIG qCfg;
    WDFDEVICE dev;
    UNREFERENCED_PARAMETER(drv);

    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnp);
    pnp.EvtDevicePrepareHardware = AsbEvtPrepareHardware;
    pnp.EvtDeviceReleaseHardware = AsbEvtReleaseHardware;
    WdfDeviceInitSetPnpPowerEventCallbacks(init, &pnp);

    /* Per-handle context + cleanup callback (no create/close handlers needed). */
    WDF_FILEOBJECT_CONFIG_INIT(&foCfg, NULL, NULL, AsbEvtFileCleanup);
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&foAttr, FILE_CONTEXT);
    WdfDeviceInitSetFileObjectConfig(init, &foCfg, &foAttr);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&devAttr, DEVICE_CONTEXT);
    st = WdfDeviceCreate(&init, &devAttr, &dev);
    if (!NT_SUCCESS(st)) return st;

    st = WdfDeviceCreateDeviceInterface(dev, &GUID_DEVINTERFACE_APPSANDBOX_SHM, NULL);
    if (!NT_SUCCESS(st)) return st;

    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&qCfg, WdfIoQueueDispatchParallel);
    qCfg.EvtIoDeviceControl = AsbEvtIoDeviceControl;
    return WdfIoQueueCreate(dev, &qCfg, WDF_NO_OBJECT_ATTRIBUTES, NULL);
}

/* Find the shared BAR (the largest CmResourceTypeMemory window = ivshmem BAR2). */
NTSTATUS AsbEvtPrepareHardware(WDFDEVICE dev, WDFCMRESLIST raw, WDFCMRESLIST trans)
{
    PDEVICE_CONTEXT ctx = DeviceGetContext(dev);
    ULONG n = WdfCmResourceListGetCount(trans), i;
    PHYSICAL_ADDRESS bestPa; SIZE_T bestLen = 0;
    UNREFERENCED_PARAMETER(raw);
    bestPa.QuadPart = 0;

    for (i = 0; i < n; i++) {
        PCM_PARTIAL_RESOURCE_DESCRIPTOR d = WdfCmResourceListGetDescriptor(trans, i);
        ULONGLONG len;
        if (d->Type == CmResourceTypeMemory) {
            len = d->u.Memory.Length;
        } else if (d->Type == CmResourceTypeMemoryLarge) {
            if (d->Flags & CM_RESOURCE_MEMORY_LARGE_40)      len = (ULONGLONG)d->u.Memory40.Length40 << 8;
            else if (d->Flags & CM_RESOURCE_MEMORY_LARGE_48) len = (ULONGLONG)d->u.Memory48.Length48 << 16;
            else if (d->Flags & CM_RESOURCE_MEMORY_LARGE_64) len = (ULONGLONG)d->u.Memory64.Length64 << 32;
            else len = d->u.Memory.Length;
        } else {
            continue;
        }
        if (len > bestLen) { bestLen = (SIZE_T)len; bestPa = d->u.Memory.Start; }
    }
    if (bestLen == 0) return STATUS_DEVICE_CONFIGURATION_ERROR;

    ctx->bar2Pa  = bestPa;
    ctx->bar2Len = bestLen;
    /* Optional kernel mapping (write-combined for write throughput). Not required for the
       user mapping, but handy and validates the range is mappable. */
    ctx->bar2SysVa = MmMapIoSpaceEx(bestPa, bestLen, PAGE_READWRITE | PAGE_WRITECOMBINE);
    return STATUS_SUCCESS;
}

NTSTATUS AsbEvtReleaseHardware(WDFDEVICE dev, WDFCMRESLIST trans)
{
    PDEVICE_CONTEXT ctx = DeviceGetContext(dev);
    UNREFERENCED_PARAMETER(trans);
    if (ctx->bar2SysVa) { MmUnmapIoSpace(ctx->bar2SysVa, ctx->bar2Len); ctx->bar2SysVa = NULL; }
    return STATUS_SUCCESS;
}

VOID AsbEvtIoDeviceControl(WDFQUEUE q, WDFREQUEST req, size_t outLen, size_t inLen, ULONG code)
{
    WDFDEVICE dev = WdfIoQueueGetDevice(q);
    PDEVICE_CONTEXT ctx = DeviceGetContext(dev);
    NTSTATUS st = STATUS_SUCCESS;
    size_t info = 0;
    UNREFERENCED_PARAMETER(inLen);

    switch (code) {
    case IOCTL_APPSANDBOX_SHM_INFO:
    case IOCTL_APPSANDBOX_SHM_MAP: {
        APPSANDBOX_SHM_MAP *out = NULL;
        WDFFILEOBJECT fo;
        PFILE_CONTEXT fc;
        PMDL mdl;
        PPFN_NUMBER pfn;
        PFN_NUMBER base;
        ULONG pages, i;
        PVOID userVa = NULL;

        if (outLen < sizeof(*out)) { st = STATUS_BUFFER_TOO_SMALL; break; }
        st = WdfRequestRetrieveOutputBuffer(req, sizeof(*out), (PVOID *)&out, NULL);
        if (!NT_SUCCESS(st)) break;
        out->size = ctx->bar2Len;
        out->userVa = 0;
        if (code == IOCTL_APPSANDBOX_SHM_INFO) { info = sizeof(*out); break; }

        fo = WdfRequestGetFileObject(req);
        fc = fo ? FileGetContext(fo) : NULL;
        if (!fc) { st = STATUS_INVALID_HANDLE; break; }
        if (fc->userVa) { out->userVa = (unsigned __int64)fc->userVa; info = sizeof(*out); break; }

        mdl = IoAllocateMdl(NULL, (ULONG)ctx->bar2Len, FALSE, FALSE, NULL);
        if (!mdl) { st = STATUS_INSUFFICIENT_RESOURCES; break; }
        /* Describe the MMIO physical pages explicitly (this is an I/O-space range, not pool). */
        pfn   = MmGetMdlPfnArray(mdl);
        base  = (PFN_NUMBER)(ctx->bar2Pa.QuadPart >> PAGE_SHIFT);
        pages = (ULONG)(ctx->bar2Len >> PAGE_SHIFT);
        for (i = 0; i < pages; i++) pfn[i] = base + i;
        mdl->MdlFlags |= MDL_PAGES_LOCKED;

        /* Cached (write-back) mapping: guest vCPU and host CPU are cache-coherent on Apple
         * Silicon (shared SLC/fabric), so a cached mapping of this RAM-backed BAR stays
         * coherent with the host's view AND reaches native memcpy bandwidth (write-combined
         * is ~6-10x slower). MmCached is the throughput choice; MmWriteCombined is the safe
         * fallback if coherency ever proves a problem. */
        __try {
            userVa = MmMapLockedPagesSpecifyCache(mdl, UserMode, MmCached,
                                                  NULL, FALSE, NormalPagePriority);
        } __except (EXCEPTION_EXECUTE_HANDLER) { userVa = NULL; }

        if (!userVa) { IoFreeMdl(mdl); st = STATUS_INSUFFICIENT_RESOURCES; break; }
        fc->mdl = mdl; fc->userVa = userVa;
        out->userVa = (unsigned __int64)userVa;
        info = sizeof(*out);
        break;
    }
    case IOCTL_APPSANDBOX_SHM_UNMAP: {
        WDFFILEOBJECT fo = WdfRequestGetFileObject(req);
        PFILE_CONTEXT fc = fo ? FileGetContext(fo) : NULL;
        if (fc && fc->userVa && fc->mdl) {
            MmUnmapLockedPages(fc->userVa, fc->mdl);
            IoFreeMdl(fc->mdl);
            fc->userVa = NULL; fc->mdl = NULL;
        }
        break;
    }
    default:
        st = STATUS_INVALID_DEVICE_REQUEST;
        break;
    }
    WdfRequestCompleteWithInformation(req, st, info);
}

VOID AsbEvtFileCleanup(WDFFILEOBJECT fo)
{
    PFILE_CONTEXT fc = FileGetContext(fo);
    if (fc && fc->userVa && fc->mdl) {
        /* Runs in the closing process's context -> safe to unmap the user VA. */
        MmUnmapLockedPages(fc->userVa, fc->mdl);
        IoFreeMdl(fc->mdl);
        fc->userVa = NULL; fc->mdl = NULL;
    }
}
