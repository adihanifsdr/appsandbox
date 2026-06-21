# Building QEMU with ivshmem-plain on macOS/HVF (AppSandbox shared-memory transport)

**Goal:** enable the standard `ivshmem-plain` PCI device (a dedicated, power-of-2-sized
shared-memory BAR) in QEMU on macOS, so a Windows guest and the macOS host can share a
small RAM region (frame buffers + control) — without exposing all guest RAM and without the
disk-backed-RAM slowness.

## Why it's not in stock QEMU on macOS (validated)
- Stock Homebrew QEMU 11.0.1 has **no** `ivshmem-plain`/`ivshmem-doorbell`, while sibling `hw/misc`
  PCI devices (`edu`, `pci-testdev`) **are** present. (Our vendored copy is no longer Homebrew bins —
  it is now a from-source build of the patched fork; see "Vendoring" below.)
- Cause: `hw/misc/Kconfig` → `config IVSHMEM_DEVICE depends on PCI && LINUX && IVSHMEM && MSI_NONBROKEN`,
  and `meson.build` sets the `IVSHMEM` symbol only via `have_ivshmem = config_host_data.get('CONFIG_EVENTFD')`.
  macOS has no `eventfd` → `IVSHMEM` off, plus the explicit `LINUX` gate.
- **But the gate is conservative.** `ivshmem-pci.c`'s plain path (`ivshmem_common_realize`)
  only does `host_memory_backend_get_memory()` + `pci_register_bar()` — no eventfd, no
  syscalls. ALL eventfd/fd-passing/KVM-irqfd lives in the doorbell/server path
  (`process_msg`, `ivshmem_add_eventfd`, `ivshmem_enable_irqfd`), which `ivshmem-plain`
  never reaches. The file uses only QEMU's cross-platform `EventNotifier`.

## Patches (against qemu-11.0.1 source)

1. **`hw/misc/Kconfig`** — drop the `LINUX` gate on the PCI device:
   ```
   -    depends on PCI && LINUX && IVSHMEM && MSI_NONBROKEN
   +    depends on PCI && IVSHMEM && MSI_NONBROKEN
   ```
   (Leave `IVSHMEM_FLAT_DEVICE` gated on LINUX — we don't build it.)

2. **`meson.build`** (~line 3282) — force the device on, keep the eventfd-only contrib
   client/server gated separately:
   ```
   -have_ivshmem = config_host_data.get('CONFIG_EVENTFD')
   +have_ivshmem_server = config_host_data.get('CONFIG_EVENTFD')
   +have_ivshmem = true
   ```
   and (~line 4568):
   ```
   -  if have_ivshmem
   +  if have_ivshmem_server
        subdir('contrib/ivshmem-client')
        subdir('contrib/ivshmem-server')
      endif
   ```

3. **`util/event_notifier-posix.c`** — define `event_notifier_init_fd()` unconditionally
   (was `#ifdef CONFIG_EVENTFD`). Its body just stores the fd; only the dead doorbell path
   calls it, but it must LINK for `ivshmem-pci.c`. Remove the `#ifdef CONFIG_EVENTFD` /
   `#endif` around the function.

## Build (macOS, Apple Silicon)
```
brew install meson ninja pkg-config            # build-time tools
cd qemu-11.0.1 && mkdir build && cd build
export PKG_CONFIG_PATH="/opt/homebrew/lib/pkgconfig:/opt/homebrew/opt/glib/lib/pkgconfig:/opt/homebrew/opt/pixman/lib/pkgconfig"
../configure --target-list=aarch64-softmmu --enable-hvf --enable-slirp --enable-cocoa
ninja qemu-system-aarch64
```
Result: `build/qemu-system-aarch64` (signed) with `ivshmem-plain` + `ivshmem-doorbell`.

## Vendoring (DONE — build-from-source)
The 3 patches above now live as commits in the fork **github.com/jamesstringer90/asb-qemu @
`asb-ivshmem-11.0.1`** (qemu 11.0.1 + the 3 ivshmem commits, no local patching). The vendoring is
build-from-source: `vendor/qemu-ivshmem/build-qemu.sh` clones that fork and builds it;
`vendor/qemu-ivshmem/stage.sh` relocates the Homebrew dylibs and re-applies the
`com.apple.security.hypervisor` entitlement (HVF); the staged `vendor/qemu-ivshmem/dist` is embedded
into the app bundle. (Replaced the old "copy the Homebrew binary" approach.)

## Validation (done, 2026-06-16)
- `qemu-system-aarch64 -device ivshmem-plain,help` → device present, has `memdev=` link.
- Frozen HVF instance with `-object memory-backend-file,id=ivshm,size=64M,share=on -device
  ivshmem-plain,memdev=ivshm` → `info pci` shows `RAM controller: PCI device 1af4:1110` with
  `BAR2: 64-bit prefetchable memory`. Backing file created at 64 MiB; host mmaps it RW.

## Usage (launch with the device)
```
-object memory-backend-file,id=ivshm,size=128M,mem-path=<RAMDISK>/ivshmem.bin,share=on
-device ivshmem-plain,memdev=ivshm
```
Put the backing file on a RAM disk (or pre-create user-owned 666 so the host app can map it RW when
QEMU runs as root). The guest needs a kernel driver to map BAR2 — **our own** `appsandbox/tools/ivshmem`
KMDF driver (built/signed/installed; no third party — Looking Glass etc. disallowed). Driver maps BAR2
with **MmCached** (coherent on Apple Silicon, ~28 GB/s). Validated transport: `TODO/ivshmem-transport-runbook.md`.
