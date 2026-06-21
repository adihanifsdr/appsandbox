# AppSandbox Windows‑on‑macOS — ivshmem shared‑memory transport: reproduction runbook

**What this is:** the exact, reproducible steps that got us to a **validated end‑to‑end zero‑copy
shared‑memory transport** between a Windows‑ARM64 guest and the macOS host on Apple Silicon, using
QEMU+HVF + ivshmem + **our own** KMDF driver (no third‑party code). Plus the **PRODUCTION DELTAS**:
what is currently test/ad‑hoc and must change to ship with real signed binaries.

> Status (2026‑06‑21): transport *primitive* built + validated (cached, coherent, ~28 GB/s, ~3 µs),
> the `asb_transport` *channel layer* built + validated end‑to‑end, AND now **in production use**:
> the real agent + 5 channel EXEs + the VDD are wired to `asb_transport` (tasks #18‑20), the Mac host
> side is built (`src/backend_mac/asb_ivshmem_transport.m`, task #34), and the agent binaries + drivers
> (incl. the ivshmem driver) are baked into the disk builder (`tools/iso-patch-mac/iso-patch-mac.m`).
> All channels (ch1 agent, ch2 VDD frames, ch3 input, ch4 audio, ch5/6 clipboard, ch7 ssh, 9P=50001)
> run on the dev VM (MyAppSandbox @ 192.168.2.2), driven by the `appsandbox --headless` daemon.
> **Known gap (open):** no slot reclaim when a guest peer dies without a clean close (agent/VDD
> restart) — a heartbeat-based reclaim was implemented and then **REVERTED** (it crashed the host and
> killed the idle ch1 control channel). The shipping transport is the stable baseline; the only kept
> hardening is the SPSC ring **acquire-barrier** (`ring_read`/`ring_read_host`) for SSH integrity.
> See windows-on-mac-plan.md + asb-transport-design.md.

Companion docs: `TODO/qemu-ivshmem-build.md` (QEMU patch detail), `tools/ivshmem/README.md`
(driver), memory `[[ivshmem-driver-transport-validated]]`, `[[ivshmem-on-macos-qemu]]`,
`[[no-external-deps-constraint]]`.

---

## Why this architecture (the short version)
- Windows won't boot under Virtualization.framework (Apple's locked EFI, no viostor) → use **QEMU +
  Hypervisor.framework**, which already boots Win11 ARM64 from inbox NVMe with our EDK2.
- QEMU/macOS has **no vsock** (vhost paths need Linux eventfd/memfd) and **no `memory-backend-shm`
  host handle** (QEMU `shm_unlink`s it). The transport that works on stock‑ish QEMU is **ivshmem**:
  a PCI BAR backed by a host `memory-backend-file` the macOS host `mmap`s — true zero‑copy.
- ivshmem is gated off on macOS QEMU builds (`depends on LINUX`) but the gate is **conservative**
  (the *plain* variant uses no eventfd), so a 3‑line patch enables it.
- Nothing in user space can map a PCI BAR → we ship a **tiny KMDF driver** that maps BAR2 to a
  user VA. No third party (Looking Glass etc. disallowed).

---

## PHASE 1 — Patched QEMU with ivshmem (host: macOS, one‑time)
Full detail in `TODO/qemu-ivshmem-build.md`. Summary:
1. `curl -LO https://download.qemu.org/qemu-11.0.1.tar.xz && tar xf …`  (match the vendored version).
2. Three patches:
   - `hw/misc/Kconfig`: `IVSHMEM_DEVICE` — drop `&& LINUX`.
   - `meson.build` ~L3282: `have_ivshmem = true` (+ split `have_ivshmem_server = CONFIG_EVENTFD`;
     gate the contrib client/server subdirs on `have_ivshmem_server`).
   - `util/event_notifier-posix.c`: define `event_notifier_init_fd()` **unconditionally** (remove
     its `#ifdef CONFIG_EVENTFD`) so `ivshmem-pci.c` links.
3. `brew install meson ninja pkg-config`; `mkdir build && cd build`;
   `PKG_CONFIG_PATH=/opt/homebrew/lib/pkgconfig:/opt/homebrew/opt/glib/lib/pkgconfig:/opt/homebrew/opt/pixman/lib/pkgconfig ../configure --target-list=aarch64-softmmu --enable-hvf --enable-slirp --enable-cocoa`; `ninja qemu-system-aarch64`.
4. Validate: `qemu-system-aarch64 -device ivshmem-plain,help` lists the device; the build signs the
   binary with `com.apple.security.hypervisor` (HVF) automatically.

**This session's binary:** `/tmp/qbuild/qemu-11.0.1/build/qemu-system-aarch64` (links Homebrew dylibs
at their install paths; runs directly on the dev machine).

**Launch with ivshmem** (keeps main RAM normal/fast; only the shared region is file‑backed). Default
**128 MiB** (power‑of‑2; sparse file → only touched pages cost RAM; 4K double‑buffer + channels fit;
64 MiB is fine for 1080p‑only — first validation ran at 64 MiB):
```
-object memory-backend-file,id=ivshm,size=128M,mem-path=/tmp/ivshmem.bin,share=on
-device ivshmem-plain,memdev=ivshm
```
The macOS host then `mmap`s the backing file; guest BAR2 offset == host file offset (contiguous,
aligned). **Full dev launch command** (was `/tmp/boot_ivshmem.command`; dev‑disk paths are ephemeral —
see bottom — but the device/ivshmem/vmnet flags are the reproducible part). Note: under `sudo` QEMU
runs as root and would create the backing file root‑owned 0644 → the host app can't map it RW; so
**pre‑create it user‑owned 666** before launch:
```sh
QBIN=/tmp/qbuild/qemu-11.0.1/build/qemu-system-aarch64        # patched QEMU (PHASE 1)
Q=/Users/james/iceman/Vendor/qemu                            # vendored firmware/data/swtpm
IVS=/tmp/ivshmem.bin
rm -f "$IVS"; : > "$IVS"; chmod 666 "$IVS"                   # user-owned 666 (sudo QEMU keeps owner)
"$Q/bin/swtpm" socket --tpm2 --tpmstate dir=/tmp/tpm-old --ctrl type=unixio,path=/tmp/swtpm-old.sock &
sudo "$QBIN" -name asb-ivshmem -accel hvf -cpu host \
  -M virt,gic-version=3,highmem=on -smp 8 -m 8192 -L "$Q/share/qemu" \
  -drive if=pflash,format=raw,readonly=on,file="$Q/share/qemu/edk2-aarch64-code.fd" \
  -drive if=pflash,format=raw,file=/tmp/vars-boot14.fd \
  -object memory-backend-file,id=ivshm,size=128M,mem-path="$IVS",share=on \
  -device ivshmem-plain,memdev=ivshm \
  -device ramfb -device qemu-xhci,id=usb -device usb-kbd -device usb-tablet \
  -device nvme,drive=hdd,serial=ours-nvme,bootindex=0 -drive if=none,id=hdd,format=raw,file=/tmp/oursp14.img \
  -device nvme,drive=data,serial=ours-devdisk -drive if=none,id=data,format=raw,file=/tmp/devdisk.img \
  -netdev vmnet-bridged,id=net0,ifname=en0 -device virtio-net-pci,netdev=net0,mac=52:54:00:ab:cd:ef \
  -display cocoa -rtc base=localtime \
  -chardev socket,id=chrtpm,path=/tmp/swtpm-old.sock -tpmdev emulator,id=tpm0,chardev=chrtpm -device tpm-tis-device,tpmdev=tpm0 \
  -monitor tcp:127.0.0.1:4445,server,nowait
```
(`vmnet-bridged` needs `sudo` + en0; for plain NAT instead use `-netdev user,id=net0,hostfwd=tcp::2222-:22`
and a unix `-monitor`. Find the bridged guest IP via `arp -an | grep 52:54:0:ab:cd:ef`.)

---

## PHASE 2 — Dev VM (the build box)
- Disk `oursp14.img` (Win11 ARM64, OpenSSH + our key) on the patched QEMU; added a **128 GB sparse
  data disk** (`devdisk.img`, NVMe) → formatted **D:** to hold the toolchain.
- **Networking — vmnet‑bridged (line rate; slirp NAT caps at ~3 MB/s):** run QEMU under `sudo` with
  `-netdev vmnet-bridged,id=net0,ifname=en0 -device virtio-net-pci,netdev=net0,mac=52:54:00:ab:cd:ef`;
  monitor moved to **TCP** (`-monitor tcp:127.0.0.1:4445,server,nowait`) so it's reachable while QEMU
  runs as root. Find the guest IP by ARP on the pinned MAC (`arp -an | grep 52:54:0:ab:cd:ef`) → was
  `192.168.1.28`. Before switching off NAT: open OpenSSH on all firewall profiles
  (`Set-NetFirewallRule …SSH… -Profile Any`) and set DNS to `1.1.1.1` (LAN resolver was flaky).
- **Toolchain (to D:):** VS2022 Community bootstrapper, unattended, `--installPath D:\VS2022 --add
  Microsoft.VisualStudio.Workload.NativeDesktop --add …VC.Tools.ARM64 --includeRecommended`. Then
  **Windows SDK 10.0.26100** (came with the workload), **Spectre runtime libs** (VS modify), and the
  **WDK 26100.6584** (`https://go.microsoft.com/fwlink/?linkid=2335869`, `wdksetup.exe /quiet`).
  GOTCHA: the bootstrapper's `--wait` returns early; the setup engine keeps installing — poll for
  `cl.exe`/SDK to actually appear. SDK & WDK **build numbers must match** (26100).

---

## PHASE 3 — Build the driver + producer (on the dev VM)
**The WDK Visual Studio driver toolset (`WindowsKernelModeDriver10.0`, component
`Component.Microsoft.Windows.DriverKit`) would NOT install headlessly** (MSB8020). Workaround that
worked: **direct `cl`/`link`** against the WDK headers/libs (no `.vcxproj`/toolset).

Source: `appsandbox/tools/ivshmem/` (`asb_ivshmem.c/.h/.inf` driver; `asb_ivshmem_producer.c` test
producer). Driver: KMDF, binds `PCI\VEN_1AF4&DEV_1110` (SUBSYS match outranks the inbox "PCI standard
RAM Controller"), maps the largest memory BAR (BAR2=64 MiB) to a user VA via `IoAllocateMdl` + manual
PFN fill + `MmMapLockedPagesSpecifyCache(UserMode, **MmCached**)`. **MmCached is coherent on Apple
Silicon and ~4–5× faster than MmWriteCombined** (proven).

**Kernel build (`build_manual.cmd`):**
```
set VCT=D:\VS2022\VC\Tools\MSVC\14.44.35207
set WK=C:\Program Files (x86)\Windows Kits\10 & set SDKV=10.0.26100.0
set PATH=%VCT%\bin\Hostarm64\arm64;%PATH%
set INCLUDE=%VCT%\include;%WK%\Include\%SDKV%\ucrt;%WK%\Include\%SDKV%\km;%WK%\Include\%SDKV%\shared;%WK%\Include\wdf\kmdf\1.33
set LIB=%VCT%\lib\arm64;%WK%\Lib\%SDKV%\km\arm64;%WK%\Lib\%SDKV%\ucrt\arm64;%WK%\Lib\wdf\kmdf\arm64\1.33
cl /nologo /c /W3 /O2 /GS /D_KERNEL_MODE /DKMDF_VERSION_MAJOR=1 /DKMDF_VERSION_MINOR=33 ^
   /DNTDDI_VERSION=0x0A000010 /D_WIN64 /D_ARM64_ /DARM64 asb_ivshmem.c
link /nologo /OUT:asb_ivshmem.sys /MACHINE:ARM64 /DRIVER /SUBSYSTEM:NATIVE,"10.00" ^
   /ENTRY:FxDriverEntry /NODEFAULTLIB /RELEASE asb_ivshmem.obj ^
   WdfDriverEntry.lib wdfldr.lib ntoskrnl.lib hal.lib BufferOverflowFastFailK.lib
```
**User‑mode producer:** same env but use `um` instead of `km` includes/libs;
`cl /O2 asb_ivshmem_producer.c /link setupapi.lib`.

---

## PHASE 4 — Sign + install (CURRENTLY TEST FLOW — see PRODUCTION DELTAS)
Prereq: **test signing ON** (`bcdedit /set testsigning on` + reboot) and **Secure Boot OFF** (our
EDK2 firmware already is). Tools in `…\Windows Kits\10\bin\10.0.26100.0\{arm64,x86}`.
```
stampinf -f asb_ivshmem.inf -d "*" -a "ARM64" -v "*" -k "1.33"
inf2cat /driver:. /os:10_GE_ARM64                       # 10_GE_ARM64 = Win11 24H2/26100 ARM64
New-SelfSignedCertificate -Type CodeSigningCert -Subject "CN=AppSandbox Test Cert" -CertStoreLocation Cert:\CurrentUser\My
#  → export .cer → Import-Certificate to Cert:\LocalMachine\{Root,TrustedPublisher}
signtool sign /fd sha256 /sha1 <thumbprint> asb_ivshmem.sys asb_ivshmem.cat
pnputil /add-driver asb_ivshmem.inf /install            # rebinds the device to our driver
```
Verify: Device Manager shows **"AppSandbox ivshmem Shared Memory", Status OK, CM_PROB_NONE**; QEMU
`info pci` shows `1af4:1110` **BAR2 now decode‑enabled** (`64 bit prefetchable memory at 0xfff8000000`).

---

## PHASE 5 — Validate (the test)
Run the producer in the guest, read on the host:
```
guest:  C:\asbdrv\asb_ivshmem_producer.exe 60      # maps BAR2 via IOCTL, writes per-page frames
host:   /tmp/shm_host /tmp/ivshmem.bin              # tools/shm-test/shm_host.c, unchanged
```
Validated (M1 Pro, 200 GB/s raw, ~52 GB/s single‑core memcpy ceiling), **cached mapping:**
- integrity **2025/2025 pages checksum‑OK** (cached is coherent guest↔host),
- guest write **~28 GB/s**, host read **~27 GB/s** (single‑thread; ~54% of single‑core wall),
- round‑trip **~3 µs**, 0 timeouts. (Write‑combined was ~6 GB/s / 9 µs.)
- 1080p60 needs 0.475 GB/s → ~60× headroom single‑thread; multi‑thread → toward 200 GB/s.

---

## PHASE 6 — asb_transport channel layer (built + validated end‑to‑end)
The shared‑memory *channel* abstraction over ivshmem (design: `TODO/asb-transport-design.md`). The
128 MiB BAR is statically PARTITIONED — one dedicated region per service; each guest sub‑program owns
its region + thread; ssh/9P get connection slots; VDD frames in their own region. Host publishes an
`AsbShmDirectory` (BAR offset 0) the guest discovers to select `backend=ivshmem` (else defaults to
AF_HYPERV — never positively detect Hyper‑V). Lock‑free SPSC rings; slot handshake FREE→CONNECTING→
ESTABLISHED = listen/accept/connect; `asb_poll`/`asb_set_timeout` map the agent's `select`/`setsockopt`.

- **Source:** `tools/transport/asb_transport.{h,c}` (guest lib); test `tools/transport/test/asb_t_guest.c`
  (guest) + `asb_t_host.c` (Mac host harness: lays out the directory + ch1 region, connects).
- **Build gotchas (real, hit this session):** include `winsock2.h` **before** `windows.h` +
  `#define WIN32_LEAN_AND_MEAN` (else `winsock.h` 1.1 conflicts); link `advapi32.lib` (registry override)
  + `ws2_32.lib` + `setupapi.lib`. Guest build (user‑mode env from PHASE 3):
  `cl /O2 asb_t_guest.c ..\asb_transport.c /link ws2_32.lib setupapi.lib advapi32.lib`.
  Mac host: `cc -O2 -o /tmp/asb_t_host tools/transport/test/asb_t_host.c`.
- **Run (host harness FIRST so the directory exists → guest picks ivshmem):**
  `/tmp/asb_t_host /tmp/ivshmem.bin` (background), then guest `asb_t_guest.exe`.
- **Validated:** `backend=ivshmem`, slot handshake, `hello ↔ 1:ping ↔ 1:pong` → **PASS** (bidirectional
  ring transport). The backing file must be user‑owned/mappable (see the 666 pre‑create above; or
  `sudo chmod 666 /tmp/ivshmem.bin` once if QEMU already created it root‑owned).

---

## PRODUCTION DELTAS — what must change to ship signed binaries (DO NOT skip)
Everything in Phases 2–5 above is **dev/ad‑hoc**. For a shipped AppSandbox build:

1. **(DONE for test-mode) ivshmem driver signed + installed like VDD/VAD.** AppSandbox VMs run
   **test‑signed drivers with testsigning ON**: the VDD `AppSandboxVDD.{sys,inf,cat,cer}`, the VAD
   `AppSandboxVAD.*`, and now `asb_ivshmem.{sys,inf,cat,cer}` are all staged + installed in the guest
   (`certutil` the `.cer` → Root+TrustedPublisher, then the driver install). NOTE: the dev build
   test-signs each driver's `.cat` with a **per-build** cert, so a redeploy must re-import the fresh
   `.cer` before `pnputil` (else `CERT_E_UNTRUSTEDROOT`). **OPEN (ship path):** Microsoft **attestation
   signing** so drivers load WITHOUT testsigning — being wired up for the ivshmem driver the same way as
   VDD/VAD (its own Package project + a `tools/sign/submission-config.json` entry, driven by
   `sign-drivers.ps1` + `make-release.ps1`).
2. **(DONE) Driver baked into the disk.** `asb_ivshmem.{sys,inf,cat,cer}` is staged by the disk-builder
   manifest (`tools/iso-patch-mac/iso-patch-mac.m`, into `\Windows\AppSandbox\drivers\` alongside the
   VDD/VAD packages) and installed by the guest provisioning (`certutil` the `.cer` to
   Root+TrustedPublisher, then the driver install). `bcdedit testsigning on` is in the unattend for
   `test_mode`.
3. **(DONE) Patched QEMU vendored from source.** `vendor/qemu-ivshmem/build-qemu.sh` clones the fork
   github.com/jamesstringer90/asb-qemu @ `asb-ivshmem-11.0.1` (qemu 11.0.1 + the 3 ivshmem commits, no
   local patches) and builds it; `vendor/qemu-ivshmem/stage.sh` relocates the dylibs and signs the
   binary with the `com.apple.security.hypervisor` entitlement (HVF). The staged
   `vendor/qemu-ivshmem/dist` is embedded into the app bundle. (Replaced the old "copy Homebrew bin".)
4. **(LARGELY DONE) Build with regular VS2022/2026 + WDK — the toolset install is solved.** The whole
   solution — the user-mode binaries (agent, VDD, the 5 channel EXEs) AND the drivers — builds via
   `tools/sign/make-release.ps1 -Platform ARM64` (msbuild over `AppSandbox.sln`). The earlier headless
   WDK driver-toolset failure (MSB8020) is resolved: install the WDK **and** its VS "Dev17" integration
   MSI, and build with the **host-native ARM64 MSBuild** (`make-release.ps1`'s Find-MSBuild now selects
   it) so the driver post-build **ApiValidator** runs — the x86 validator returns exit 193 on an ARM64
   host. SDK & WDK build numbers must match (26100). The direct `cl`/`link` recipe (Phase 3) remains a
   driver-only fallback. Still TODO: run this in real CI rather than on the dev VM by hand.
5. **ivshmem backing store.** Dev used `/tmp/ivshmem.bin` (APFS). For shipping, put the region on a
   RAM‑backed path (RAM disk) or accept page‑cache residency. **Default size 128 MiB** (power of 2;
   sparse → only touched pages cost RAM): covers 4K double‑buffer (~63 MiB) + the channel rings
   (~2–16 MiB, dominated by 9P/ssh/clipboard) with headroom. 64 MiB suffices for 1080p‑only; bump to
   256 MiB for 4K triple‑buffer or multi‑monitor.
6. **Cache type.** Ship **MmCached** (validated coherent + fast). Keep MmWriteCombined as a
   compile‑time fallback only.
7. **DONE — the transport abstraction is wired into the real components and in production use.**
   `asb_transport` (the partitioned ring layer over the 128 MiB BAR) now backs the live system: the
   agent + 5 channel EXEs + the VDD are wired to it (their inlined AF_HYPERV sockets switched to the
   channel API — PC AF_HYPERV path kept byte-identical), the VDD **frame channel** `asb_frame_*` is
   **implemented** (ch2 BGRA double/triple-buffer + cursor, `tools/transport/asb_transport.c`), and the
   **real Mac host side** is built (`src/backend_mac/asb_ivshmem_transport.m`: lays out the directory +
   regions at VM-create, runs a per-slot pump thread per connection feeding vm_agent_mac /
   vm_ssh_proxy_mac / vm_clipboard_mac / the IDD display consumer). The old `asb_t_guest.c`/`asb_t_host.c`
   harness (above) remains only as the standalone primitive test. **Remaining open item:** robust slot
   reclaim on guest-peer death (the heartbeat approach was reverted — see Status, top).
8. **Networking in the shipped app** uses Virtualization‑style/managed networking, not the dev
   `sudo vmnet-bridged`. The bridged setup here was only to get line‑rate toolchain downloads.

## Ephemeral dev artifacts (NOT part of the product; for this session's continuity only)
`/tmp/qbuild/…/qemu-system-aarch64` (patched QEMU), `/tmp/boot_ivshmem.command`, `/tmp/oursp14.img`,
`/tmp/devdisk.img`, `/tmp/vars-boot14.fd`, `/tmp/ivshmem.bin`, `/tmp/shm_host`, swtpm `/tmp/tpm-old`,
TCP monitor `127.0.0.1:4445`, dev VM `192.168.1.28` (user/test123, key `~/.ssh/claude_remote_ed25519`),
guest build dir `C:\asbdrv`, toolchain `D:\VS2022`.
