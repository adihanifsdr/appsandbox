# AppSandbox — Windows Code Change Brief

> **Audience:** an engineer/agent working on the **Windows** side of AppSandbox (the guest binaries,
> the drivers, the Windows backend `src/backend_win`, the Windows build/sign/release pipeline).
> **Purpose:** make you fully aware of everything that changed to support running Windows guests on a
> **macOS host** (QEMU+HVF), what is new in the build/deploy process, and what remains on the Windows
> side. Most of this is **transparent to a Windows‑on‑Windows (Hyper‑V/HCS) VM** — but the Windows
> code must still know about it. Read this in full before touching guest or provisioning code.

---

## 0. The one‑paragraph summary
AppSandbox now creates Windows guest VMs on **two** kinds of host: the original **Windows host**
(Hyper‑V / HCS, transport = AF_HYPERV hvsocket) and a new **macOS host** (QEMU+HVF, transport =
**ivshmem shared memory**). To make the **same** guest binaries work on both, we added (a) a runtime
**transport abstraction** (`asb_transport`) that the guest picks at startup, (b) a **single,
platform‑neutral provisioning generator** (`win_provision.c`), and (c) **two new guest drivers** —
our **ivshmem** driver and the **NetKVM** virtio‑net driver. On a Windows‑on‑Windows VM the transport
defaults to AF_HYPERV and the two new drivers find no matching PCI device, so they are **harmless
no‑ops** — but the build, the manifest, and the provisioning scripts must still stage and reference
them, because there is **one** unified code path (no is‑mac/is‑pc fork).

---

## 1. The core change — `asb_transport` (runtime transport abstraction)

**New files (linked into EVERY guest binary):**
- `tools/transport/asb_transport.h` — the API.
- `tools/transport/asb_transport.c` — the two backends.
- `tools/transport/asb_shm_layout.h` — shared‑memory region layout (host + guest agree on it).

**One API, two backends, chosen at runtime by `asb_transport_init()`:**
1. **Explicit override** — `HKLM\SOFTWARE\AppSandbox\Transport` registry value (the Mac
   disk builder sets `ivshmem`).
2. else **positively detect ivshmem** — our driver's device interface present, via
   `SetupDiGetClassDevs(GUID_DEVINTERFACE_APPSANDBOX_SHM)`.
3. else **default to AF_HYPERV** (HCS/hvsocket) — **PC behavior byte‑identical to before**.
   **NEVER positively detect "Hyper‑V".**

**API (a drop‑in for the old socket sites):**
- Streams: `asb_listen(channel)` / `asb_accept(l, timeout_ms)` / `asb_connect(channel)` /
  `asb_recv` / `asb_send` / `asb_poll` (replaces `select`) / `asb_set_timeout` (replaces
  `setsockopt SO_*TIMEO`) / `asb_close` / `asb_close_listener`.
- Newline helpers: `asb_recv_line` / `asb_send_line`.
- VDD frame channel: `asb_frame_open` / `asb_frame_back_buffer` / `asb_frame_publish` /
  `asb_frame_cursor` / `asb_frame_close`.
- Escape hatch: `asb_conn_socket_u64(c)` returns the underlying **PC SOCKET** (or `~0` on ivshmem) so
  PC‑only paths (e.g. the SSH relay's dual‑fd `select`) stay byte‑identical.
- Channel IDs (the **N** in the PC service GUID `a5b0cafe-000N-4000-8000-000000000001`):
  `ASB_CH_AGENT 1`, `ASB_CH_DISPLAY 2`, `ASB_CH_INPUT 3`, `ASB_CH_AUDIO 4`, `ASB_CH_CLIPBOARD 5`,
  `ASB_CH_CLIPBOARD_READER 6`, `ASB_CH_SSH 7`, `ASB_CH_9P 50001`.

The shared‑memory side is a 128 MiB BAR statically partitioned into per‑channel regions
(`AsbShmDirectory` at offset 0 + per‑channel SPSC ring slots, plus a double/triple‑buffered
`AsbFrameRegion` for ch2). **You don't need to touch this on PC** — it's only used when the ivshmem
backend is active.

---

## 2. What changed in the AGENT FILES (the 8 socket sites → `asb_transport`)

Previously the guest binaries each inlined `socket(AF_HYPERV, SOCK_STREAM, 1)` +
`SOCKADDR_HV{ VmId, ServiceId = a5b0cafe-000N }` at **8 sites**. Those are now `asb_transport` calls.
**The byte protocols ABOVE the socket are UNCHANGED** — `hello`/heartbeat, `<seq>:cmd`, and the
magics (`ASIN`/`ASA1`/`ACLP`/`CLDY`/`IRDY`/`ASFR`/`ASCR`). The change is mechanical and additive:

| Old | New |
|---|---|
| `SOCKET s` | `AsbConn *s` |
| `INVALID_SOCKET` | `NULL` |
| `socket()/bind()/listen()/accept()` on a ServiceId | `asb_listen(ASB_CH_*)` + `asb_accept()` |
| `connect()` (9P) | `asb_connect(ASB_CH_*)` |
| `recv()/send()` | `asb_recv()/asb_send()` |
| `select()` | `asb_poll()` |
| `setsockopt SO_*TIMEO` | `asb_set_timeout()` |
| `closesocket()` | `asb_close()` |

**Per file:**
- **`tools/agent/agent.c`** (ch1 control + ch7 SSH) — calls `asb_transport_init()` at startup;
  control loop = `asb_listen(ASB_CH_AGENT)` + `asb_accept(l, 1000)`; `recv_line`/`send_line` now use
  `asb_recv`/`asb_send`; heartbeat uses `asb_poll`. The **SSH proxy** = `asb_listen(ASB_CH_SSH)` +
  `asb_accept`, relayed to `127.0.0.1:22`; its dual‑fd relay keeps a **PC‑only `select` branch** via
  `asb_conn_socket_u64()` so PC SSH behavior is unchanged. (Net: ~260 lines of socket boilerplate
  removed across the agent.)
- **`tools/agent/appsandbox-input.c`** (ch3), **`-audio.c`** (ch4), **`-clipboard.c`** (ch5),
  **`-clipboard-reader.c`** (ch6) — each is now `asb_listen(ASB_CH_*)` + `asb_accept` + `asb_recv`/
  `asb_send`. The actual logic (SendInput injection, WASAPI loopback capture, clipboard format
  conversion) is **unchanged**; only the transport wrapper changed, so each file shrank substantially.
- **`tools/agent/p9copy.c`** (ch50001) — guest→host **connect‑out** via `asb_connect(ASB_CH_9P)` on
  the ivshmem path. The **PC AF_HYPERV path is kept intact** (branch on `is_ivshmem`); do not remove it.
- **`tools/vdd/vdd.cpp`** (ch2 frame) — an **additive** ivshmem frame path via
  `asb_frame_open`/`asb_frame_back_buffer`/`asb_frame_publish`/`asb_frame_cursor` (or
  `asb_listen(ASB_CH_DISPLAY)`/`asb_accept`). On PC the existing ch2 hvsocket path is **byte‑identical**
  (the frame handle is NULL → the HvSocket path runs).

**Build impact:** every one of these binaries now `#include "../transport/asb_transport.h"` and must
**link `tools/transport/asb_transport.c`**. Add it to each `.vcxproj`'s sources.

---

## 3. NEW — the centralized config builder (`win_provision.c` / `.h`)

**Files:** `tools/provision/win_provision.h` + `win_provision.c` — **one platform‑neutral generator**
(no Win32, no Foundation) for `unattend.xml` + `setup.cmd` + `SetupComplete.cmd`. It is the **single
source of truth**, compiled into **BOTH** the Windows backend AND the macOS `iso-patch-mac` target, so
the staged scripts are **byte‑identical regardless of host**. **There is no is‑mac/is‑pc fork.**

**API:**
- `asb_provision_unattend(f, vm_name, user, pass, arch, test_mode, is_arm64, lang)` — specialize pass
  (BypassNRO, recovery off, `bcdedit /set testsigning on` when `test_mode`, ARM64 `LabConfig`
  TPM/SecureBoot/RAM/Storage/CPU bypass) + oobeSystem (locale, local admin, AutoLogon, FirstLogon →
  `C:\Windows\AppSandbox\setup.cmd`).
- `asb_provision_setup_cmd(f)` — first logon: `appsandbox-agent.exe --install` (the agent is already
  staged at `C:\Windows\AppSandbox\`).
- `asb_provision_setupcomplete(f, ssh_msi_name)` — runs as SYSTEM before first logon. **Install order:**
  1. `bcdedit /set testsigning on`
  2. **AppSandboxSHM** — `certutil` trust `AppSandboxSHM.cer` (its OWN "AppSandbox Test Cert"), then
     `devcon update AppSandboxSHM.inf "PCI\VEN_1AF4&DEV_1110&SUBSYS_11001AF4&REV_01"`
  3. **VDD** — `certutil` trust `AppSandboxVDD.cer` (WDKTestCert), then `devcon install ... Root\AppSandboxVDD`
  4. disable display sleep (`powercfg`)
  5. **VAD** — `devcon install AppSandboxVAD.inf Root\AppSandboxVAD` (Microsoft WHQL‑signed, no cert)
  6. **NetKVM** — `devcon update netkvm.inf "PCI\VEN_1AF4&DEV_1000&SUBSYS_00011AF4&REV_00"` (Red Hat
     WHQL‑signed, no cert; `netkvmp.exe` must sit beside the INF)
  7. **OpenSSH** — `msiexec` + `sc config sshd start=auto` + `net start sshd` (if an MSI is staged)

The **ivshmem (step 2)** and **NetKVM (step 6)** steps are **`if exist`‑guarded and a no‑op on
Win→Win** (no matching PCI device), so it's safe to run them unconditionally.

---

## 4. NEW — two guest drivers the Windows code must know about

### 4a. ivshmem driver — `tools/ivshmem/AppSandboxSHM.{c,h,inf,vcxproj}` (ours, no third party)
- **What:** a minimal **KMDF** driver (`Class = System`) that binds QEMU's **ivshmem‑plain** PCI
  device and maps its **BAR2** (the shared‑memory window) into a user VA via `IOCTL_APPSANDBOX_SHM_MAP`.
  This is the macOS transport substrate (`asb_transport`'s ivshmem backend).
- **Hardware ID:** `PCI\VEN_1AF4&DEV_1110` (SUBSYS `11001AF4&REV_01` most‑specific first, so it
  outranks the inbox "PCI standard RAM Controller"). **Catalog `AppSandboxSHM.cat`**, signed by the
  **separate "AppSandbox Test Cert"** (NOT the VDD's WDKTestCert — three drivers, three certs).
- **User‑mode discovery:** `GUID_DEVINTERFACE_APPSANDBOX_SHM`
  (`69d97ae1-ad44-4a12-88db-e5d581890ef1`) — this is what `asb_transport_init()` looks up.
- **On Windows‑on‑Windows:** there is **no ivshmem PCI device**, so the driver never binds and the
  `devcon update` is a no‑op; the transport falls back to AF_HYPERV. It is shipped/installed anyway
  because the provisioning path is unified.

### 4b. network driver — NetKVM / virtio‑net — downloaded at VM-create (third party, NOT committed to the repo)
- **What:** Red Hat **virtio‑win NetKVM** (`netkvm.{inf,sys,cat}` + `netkvmp.exe`), **BSD‑3‑Clause**,
  **WHQL‑signed** (installs without test signing). It drives QEMU's `virtio-net-pci` for NAT
  networking on the Mac path (Windows ARM64 has **no inbox virtio‑net driver**).
- **Hardware ID:** `PCI\VEN_1AF4&DEV_1000`. Installed via **`devcon update`** (binds to the existing
  device). `netkvmp.exe` must sit beside the INF (the INF CopyFiles‑es it).
- **On Windows‑on‑Windows:** Hyper‑V provides a synthetic NIC, there is **no virtio‑net device**, so
  `devcon update` is a no‑op. Shipped/referenced anyway for the unified path.
- **Sourcing:** **downloaded at VM-create** (~789 MB virtio-win; `src/backend_mac/asb_core_mac.m` —
  "Downloading guest drivers"), cached locally and staged into the guest, NOT committed to the repo
  (no `vendor/virtio-win` in git). License `virtio-win_license.txt` ships beside it; it's the
  only added third‑party guest binary, BSD‑3‑Clause (notice only).

---

## 5. NEW files in the build & deploy process (checklist)
- `tools/transport/asb_transport.{c,h}` + `asb_shm_layout.h` — **link into every guest binary**.
- `tools/provision/win_provision.{c,h}` — **compile into the Windows app project** (and the Mac
  target). Replaces the inline script generators in `disk_util.c`.
- `tools/ivshmem/AppSandboxSHM.{c,h,inf,vcxproj}` — driver #1; build → `AppSandboxSHM.{sys,inf,cat}` +
  extract `AppSandboxSHM.cer`; **sign + emit into `release/resources/drivers`**.
- NetKVM (`netkvm.*`, `netkvmp.exe`) — **downloaded at VM-create, not committed** — driver #2; **copy into
  `release/resources/drivers`** in the package step (already WHQL‑signed).
- The Windows release manifest (`generate_vhdx_manifest`) must stage **both** new drivers + their
  certs alongside the VDD/VAD payload.

---

## 6. WHAT REMAINS on the Windows side

### R5 — Windows‑side parity for the unified generator (NOT done; build on the Win dev VM)
Today `src/backend_win/disk_util.c` still generates its **own inline** `unattend.xml` /
`setup.cmd` / `SetupComplete.cmd` (functions `generate_unattend_vhdx`, `generate_vhdx_setup_cmd`,
`generate_vhdx_setupcomplete`, `generate_vhdx_manifest`, `stage_agent_and_setup`) and **does not yet**:
- delegate to `win_provision.c` (so Win→Win and Win→Mac would diverge);
- include the **AppSandboxSHM** + **NetKVM** install steps in its SetupComplete;
- add `AppSandboxSHM.{inf,sys,cat,cer}` (and `netkvm.*` + `netkvmp.exe`) to `generate_vhdx_manifest`.

**To do:** compile `win_provision.c` into the Windows app `.vcxproj`; replace `disk_util.c`'s inline
generators with `asb_provision_unattend` / `asb_provision_setup_cmd` / `asb_provision_setupcomplete`;
add the ivshmem (+ netkvm) driver files to the manifest; have `tools/ivshmem/AppSandboxSHM.vcxproj`
emit into `release/resources/drivers` and `sign-drivers.ps1` sign it. This keeps **Win→Win
byte‑identical** and proves a release‑signed build reproduces the Mac result.

### Deferred guest‑agent bug — clean shutdown on power‑off (open; affects Win→Win AND Win→Mac)
The agent service + its **4 spawned channel helpers do not exit on a Windows shutdown**, so the guest's
ACPI power‑off times out. On the Mac path the host stop sends ACPI `system_powerdown`; if the agent
hangs, the guest takes the full ACPI timeout or gets force‑killed (risking a dirty NTFS volume).
**UPDATE — now handled (verify helper teardown):** the agent honors `SERVICE_CONTROL_SHUTDOWN`
(`tools/agent/agent.c` `service_ctrl` ~L2374 — sets `g_os_shutting_down`, sends `os_shutdown` to the
host); confirm the spawned channel helpers also exit promptly. Fallback workaround: `shutdown /s /f`. **Fix:** the agent must honor `SERVICE_CONTROL_SHUTDOWN` (and the
helpers `WM_QUERYENDSESSION` / `WM_ENDSESSION`), tearing down their `asb_transport` connections + threads
promptly so every process exits before ACPI fires.

### ⛔ SHIP BLOCKER (see the top of `TODO/windows-on-mac-plan.md`)
The guest payload is **unsigned / test‑signed** today (the agent EXEs are "not signed at all"; the
drivers carry our own test certs trusted only in **testMode**). It installs on a **test‑signed guest
only**. To ship, the guest binaries must EITHER be **EV + attestation/WHQL‑signed** (so they install
on a normal non‑test‑signed Windows) **OR** be **dynamically pulled from a signed official ARM64
release** at VM‑build time. **Compounding:** the current **0.1.3 Windows release does NOT contain the
ivshmem driver or the asb_transport‑wired agent/channel/VDD binaries**, so a **new signed Windows
release must be cut** that includes them before either ship path works.

---

## 7. Invariants — DO NOT BREAK
- **Default = AF_HYPERV.** Detect ivshmem **positively**; otherwise default to AF_HYPERV. Never
  positively detect "Hyper‑V". Win→Win must stay byte‑identical.
- **Byte protocols above the transport are frozen** — don't change the handshake/magics/`<seq>:cmd`.
- **ivshmem + NetKVM install steps stay unconditional no‑ops** on Win→Win (no is‑mac fork; `if exist`
  guards).
- **Keep p9copy's PC AF_HYPERV branch** (`is_ivshmem ? asb_* : socket`).
- **Keep the SSH relay's PC `select` branch** via `asb_conn_socket_u64()` (zero PC behavior change).
- **Three drivers, three certs** — VDD = WDKTestCert; VAD = Microsoft WHQL (already trusted);
  AppSandboxSHM = "AppSandbox Test Cert". Each test‑signed driver needs its OWN `.cer` trusted before
  install or SetupComplete hangs on an untrusted publisher.
