/*
 * asb_ivshmem.h — shared contract between the AppSandbox ivshmem KMDF driver and the
 * user-mode clients (agent / VDD / test app). No third-party code.
 *
 * The driver binds the QEMU ivshmem-plain PCI device (VEN_1AF4&DEV_1110), maps its BAR2
 * (the shared-memory window backed by the macOS host's memory-backend-file) and hands a
 * user-mode mapping of that BAR to the caller. The macOS host mmaps the same backing file,
 * so this is a true zero-copy host<->guest shared region.
 */
#pragma once

#ifdef _KERNEL_MODE
#include <ntddk.h>
#else
#include <windows.h>
#include <winioctl.h>
#endif

#include <initguid.h>

/* Device interface GUID — user mode locates the device via SetupDiGetClassDevs(this). */
DEFINE_GUID(GUID_DEVINTERFACE_ASB_IVSHMEM,
    0x69d97ae1, 0xad44, 0x4a12, 0x88, 0xdb, 0xe5, 0xd5, 0x81, 0x89, 0x0e, 0xf1);

#define ASB_IVSHMEM_DEVTYPE 0x8123u  /* custom device type for CTL_CODE */

/* Map BAR2 into the caller's address space. Out: ASB_IVSHMEM_MAP. */
#define IOCTL_ASB_IVSHMEM_MAP   CTL_CODE(ASB_IVSHMEM_DEVTYPE, 0x801, METHOD_BUFFERED, FILE_ANY_ACCESS)
/* Explicit unmap (also performed automatically when the handle is closed). */
#define IOCTL_ASB_IVSHMEM_UNMAP CTL_CODE(ASB_IVSHMEM_DEVTYPE, 0x802, METHOD_BUFFERED, FILE_ANY_ACCESS)
/* Query the region size without mapping. Out: ASB_IVSHMEM_MAP (userVa=0). */
#define IOCTL_ASB_IVSHMEM_INFO  CTL_CODE(ASB_IVSHMEM_DEVTYPE, 0x803, METHOD_BUFFERED, FILE_ANY_ACCESS)

#pragma pack(push, 1)
typedef struct _ASB_IVSHMEM_MAP {
    unsigned __int64 size;    /* [out] BAR2 size in bytes (the shared region)            */
    unsigned __int64 userVa;  /* [out] user-mode VA of the mapped region (0 on failure)  */
} ASB_IVSHMEM_MAP;
#pragma pack(pop)
