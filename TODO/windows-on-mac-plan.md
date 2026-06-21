# Windows AppSandbox VMs on macOS — Implementation Plan (living document)

> # ⛔ DO NOT SHIP — CRITICAL BLOCKER: Windows guest payload is unsigned / test‑signed
>
> **AppSandbox CANNOT ship to the public in its current state.** The Windows guest payload baked into
> the macOS bundle (`Contents/Resources/agent_win` ← committed `tools/agent_win`: the agent EXEs +
> channel helpers + the VDD / **ivshmem** / VAD drivers) is **unsigned or test‑signed**. Verified:
> `appsandbox-agent.exe` = "code object is not signed at all"; the drivers carry our **own test certs**
> (`asb_ivshmem.cer`, `AppSandboxVDD.cer`) that are trusted via `certutil` only because the guest boots
> **testMode** (test‑signing on, Secure Boot off). This payload installs on a **test‑signed guest only**.
>
> To ship, the guest binaries must EITHER:
> 1. be **EV + attestation/WHQL‑signed** (drivers) and Authenticode **EV‑signed** (agent + channel EXEs)
>    so they install on a **normal, non‑test‑signed** Windows guest (then testMode is no longer required
>    for signing); **OR**
> 2. be **dynamically pulled at VM‑build time** from our **latest official ARM64 Windows release** and
>    extracted — i.e. ship only signed, released binaries, never the test‑signed dev payload (the
>    `windows_payload_directory()` / JIT seam, REMAINING WORK **R9**).
>
> **SECOND, COMPOUNDING BLOCKER:** the current **0.1.3 Windows ARM64 release does NOT contain the
> ivshmem driver or the ivshmem‑transport‑wired agent / channel binaries.** So option 2 cannot work yet
> — there is **no released signed build to pull the ivshmem payload from**. A new Windows release must
> first be cut that includes the ivshmem driver + the ivshmem‑wired agent + channel EXEs (Windows‑side
> parity, **R5**, feeds this).
>
> **Net:** today the macOS bundle ships **unsigned/test‑signed Windows binaries that require testMode**,
> and there is **no signed released build to pull from**. BOTH must be resolved before any public ship.
> (The macOS side — app, framework, QEMU + dylibs — IS Developer‑ID signed + notarized; this blocker is
> entirely the **Windows guest payload**.)

> Status: **FROM‑SCRATCH WINDOWS VM BUILT FROM macOS, BOOTS, AGENT ONLINE, SSH IN — end to end via the
> product API.** (See "FROM‑SCRATCH VM WORKING" below.) A `osType="Windows"` VM is created from a
> stock MS‑store ISO entirely by our own code (no DISM): iso‑patch‑mac mounts the ISO, our NTFS writer
> applies `install.wim` and stages the payload, the unified provisioning generator bakes
> unattend/setup.cmd/SetupComplete (scripts = main + only the ivshmem driver), QEMU+HVF boots it
> testMode with `vmnet-shared`, the per‑driver certs + drivers install, and **the agent comes online via
> main's unmodified FirstLogon (~7 min) with SSH key login (sshState 4).**
> Already done + measured below: host substrate (QEMU+HVF), the shared‑memory primitive (patched QEMU
> ivshmem + our own KMDF driver, ~28 GB/s cached‑coherent), the dev toolchain, the `asb_transport`
> channel layer, the full interactive feature set (display/cursor/input/audio/clipboard over ivshmem),
> and now the from‑scratch macOS build/boot/provision flow.
> **What's left → see "REMAINING WORK" near the bottom.** The 2026‑06‑17 batch closed the display host
> consumer (R1 — `src/backend_mac/idd_display.m` `IddDisplayWindow`, which carries display+cursor ch2,
> input ch3, audio ch4, and clipboard ch5/ch6 in one host window), GUI enablement (R2), guest NIC (R4 —
> virtio‑net‑pci + NetKVM), ssh ch7 hardening (R6), the delete‑crash (R8), and payload staging (R7) — all
> building. The big ones now remaining: **Windows‑side parity** for the unified generator (R5), acceptance
> automation (R10), **robust ivshmem slot‑reclaim across agent/VDD restart** (R11 — open), and a
> **runtime QA pass**. Reproduction + production‑signing: `TODO/ivshmem-transport-runbook.md`.

## Goal & acceptance
- **Goal:** launch full Windows AppSandbox VMs from macOS with parity to Windows‑hosted VMs
  **except GPU‑PV** (not possible on macOS). The VDD is **kept** (future GPU accel); only **how**
  components communicate changes per host (PC = AF_HYPERV/HCS; Mac = ivshmem shared memory). The
  QEMU display is temporary and removed later.
- **Definition of done:** on a macOS host, `tools/headless-api/tests/run_all.py` runs
  `vm_lifecycle.py` for an `osType="Windows"` spec (`testMode=True, sshEnabled, sshDeployKey,
  user/test123`) and passes: build → **boot‑to‑online (agent over the Mac transport)** →
  **display_ready latches (VDD `idd_status:ok`)** → **SSH key deployed (`sshState 4`) + key‑only
  login** → graceful (agent) shutdown → force‑stop → delete; snapshots → **501**. Display
  open/close skipped on Mac, but `display_ready` must be true.

---

## BUILT & VALIDATED (2026‑06‑16) — the ivshmem transport
Full reproduction + production deltas: `TODO/ivshmem-transport-runbook.md`. Memory:
`[[ivshmem-driver-transport-validated]]`, `[[ivshmem-on-macos-qemu]]`.

- **Substrate:** QEMU + Hypervisor.framework boots Win11 ARM64 to desktop from inbox NVMe with our
  EDK2 (Iceman; `oursp14`). VZ was tested and rejected (Apple's locked EFI won't boot Windows).
- **Patched QEMU 11.0.1 with `ivshmem-plain` on macOS/HVF** — ivshmem is gated off on macOS
  (`depends on LINUX` + eventfd), but the gate is **conservative** (plain variant uses no eventfd);
  a 3‑line patch (Kconfig, meson, `event_notifier_init_fd`) builds it. Validated: PCI `1af4:1110`,
  64 MiB BAR2, host `mmap`s the `memory-backend-file` backing. (`TODO/qemu-ivshmem-build.md`.)
- **Our own KMDF driver** (`tools/ivshmem/asb_ivshmem.c/.h/.inf`, no third‑party): binds
  `VEN_1AF4&DEV_1110`, maps BAR2 to a user VA via IOCTL. Built (direct `cl`/`link`), test‑signed,
  installed → Device Manager "AppSandbox ivshmem Shared Memory", Status OK; BAR2 decode‑enabled.
- **End‑to‑end zero‑copy proven:** guest producer writes via the driver; macOS host `mmap`s the same
  file and reads identical bytes (epoch/magic matched). **Cached mapping is coherent on Apple
  Silicon** (integrity 2025/2025 pages) AND fast: **~28 GB/s guest‑write, ~27 GB/s host‑read
  (single‑thread), ~3 µs round‑trip.** No virtualization tax on the data path; only the per‑core
  memcpy wall (~52 GB/s). 1080p60 needs 0.475 GB/s → ~60× headroom; multi‑thread → toward 200 GB/s.
- **Dev toolchain box (done):** dev VM (`oursp14`) on the patched QEMU, `vmnet-bridged` for
  line‑rate, **VS2022 + Windows SDK 26100 + WDK 26100.6584** on a 128 GB data disk (D:). Test
  signing on, Secure Boot off.

**Why ivshmem (not vsock/virtio‑serial/vhost‑user):** QEMU/macOS has no working vsock (vhost needs
Linux eventfd/memfd) and `memory-backend-shm` is anonymous (QEMU `shm_unlink`s it → no host handle).
ivshmem + `memory-backend-file` is the only stock‑QEMU path that gives the host a mappable handle to
shared guest memory. Only constraint allowed by the user: external deps = **QEMU + EDK2 only**
(`[[no-external-deps-constraint]]`) → we write our own driver.

---

## INTERACTIVE VM WORKING (2026‑06‑16 session 2) — display + input over ivshmem
A live, interactive Windows desktop now renders in a macOS window and accepts mouse/keyboard, entirely
over the ivshmem transport, with **no QEMU display window**. Built from the real product binaries
(modified in place, not cloned).

- **Display — DONE.** The real VDD (`tools/vdd/vdd.cpp`) gained an *additive* ivshmem frame path
  (gated by `g_asbFrame`; NULL on PC ⇒ HvSocket path byte‑identical). It publishes each captured BGRA
  frame into the ch2 `AsbFrameRegion`. **Dirty‑rect handling:** IddCx only guarantees the *dirty*
  regions of an acquired surface are current, so the VDD keeps a **persistent accumulator** (full copy
  on the first frame of a swap chain, then `IddCxSwapChainGetDirtyRects` patches) and publishes the
  whole coherent frame — the Mac consumer stays trivial. Validated: real Win11 desktop, ~40 fps when
  active, quiescent when idle (no wasteful idle‑resend on the Mac route).
- **Mac viewer — DONE (test host).** `tools/transport/test/asb_viewer.m` (Cocoa, `clang -framework
  Cocoa`): mmaps `/tmp/ivshmem.bin`, **publishes the directory** (host role) with a ch2 FRAME region +
  a ch3 INPUT stream slot, renders the active buffer at 60 fps, and captures NSEvents → `InputPacket`s.
  Mouse moves are **coalesced to ≤1 per render tick** and flushed before discrete events (a retina
  trackpad otherwise floods the 204‑slot ring → drops + growing latency). Kept independent per the
  user (the production Mac host lives in `src/backend_mac`, Workstream D).
- **Input — DONE.** The real `appsandbox-input.c` was wired to `asb_transport` (`asb_listen(ASB_CH_INPUT)`
  /`asb_accept`/`asb_recv` — same Hyper‑V GUID on PC, ivshmem slot on Mac; injector unchanged). The
  viewer is the slot **connector**, the agent‑spawned helper is the **acceptor**. `asb_transport`
  gained `asb_transport_region_base()`/`asb_ring_drain()` for raw one‑directional regions.
- **Agent built + deployed the real‑provisioning way — DONE.** `appsandbox-agent.exe` built with VS2022
  (vanilla `cl`, no tweaks), deployed to **`C:\Windows\AppSandbox\`** and installed as the
  **`AppSandboxAgent`** service (`--install`, LocalSystem). Its monitor threads spawn the channel
  helpers into the **console session** via `CreateProcessAsUserW` — independent of the (still
  AF_HYPERV) control channel, so input works even though task #18 is pending. Repo scripts:
  `tools/agent/build-arm64.cmd`, `tools/agent/deploy-arm64.cmd`.
- **Headless — DONE.** Boot with **`-display none`** (the QEMU cocoa window stole/serialized input and
  caused lag). The guest keeps ramfb (early boot) + the VDD (desktop); `asb_viewer` is the only
  display. Boot via `open -a Terminal /tmp/boot_ivshmem.command` (`-smp 4 -m 4096`).
- **Build toolchain — locked in.** User‑mode guest binaries → **VS2022** (registered at `D:\VS2022`,
  enter the dev prompt via `vswhere`+`VsDevCmd`, NO INCLUDE/LIB hacks). VDD only → **EWDK** (`E:`, UMDF/
  IddCx targets). Never redirect `VsDevCmd`/`SetupBuildEnv` to `>nul` (breaks the env). Memory:
  `[[vm-build-toolchain]]`, `[[ivshmem-viewer-display-none]]`.

- **Cursor — DONE.** The captured frame never contains the HW cursor, so the VDD copies the Windows
  cursor (bitmap + hotspot + type + visibility) into a dedicated **cursor area** of the frame region
  (`AsbCursor` at `cursor_offset`; `VddWriteIvshmemCursor` mirrors `VddSendCursorUpdate`'s
  `IddCxMonitorQueryHardwareCursor3`). The viewer sets it as the view's **`NSCursor`** (NOT drawn into
  the frame), so macOS renders it at the real pointer position with the correct hotspot. Both
  QueryHardwareCursor3 types decode (mirroring `vm_display_idd.c`): ALPHA = premultiplied BGRA;
  MASKED_COLOR = BGR with the AND mask in alpha, and XOR/invert pixels (the I-beam strokes) approximated
  as opaque black. Sized at **content scale** (`px × window/guest`) so it's correct on a retina host vs
  the VM's 100%. Hidden guest cursor → transparent NSCursor. Validated (arrow, I-beam, resize, hand).

**Driver redeploy gotcha:** same `DriverVer` (single‑source `$(AsbVersion)`) means `devcon install`
keeps the old driver‑store package → a stale UMDF DLL keeps running. Proper reinstall:
`devcon remove` → `pnputil /delete-driver oemNN.inf /uninstall /force` → `devcon install`. NEVER hand‑copy
the DLL into `System32\drivers\UMDF`. Also: the EWDK ISO at `E:` must be re‑mounted after each VM reboot
(`Mount-DiskImage D:\ewdk.iso`) or the VDD build fails with msbuild‑not‑found.

- **Audio — DONE.** The real `appsandbox-audio.c` was wired to `asb_transport` (`asb_listen(ASB_CH_AUDIO)`;
  WASAPI loopback capture of the "App Sandbox Virtual Audio" endpoint unchanged). It streams an
  `AudioHeader` then `AudioFrameHeader`+PCM into the slot's **g2h** ring (guest→host). The viewer publishes
  a ch4 stream slot (256 KiB rings), is the connector, drains g2h on a reader thread into a 1 MiB PCM
  jitter buffer, and plays it via a **CoreAudio AudioQueue** (format from the header: 44.1 kHz/2ch/float).
  The VM's virtual-audio driver (installed by the user) provides the endpoint. Validated: VM audio heard
  on the Mac. (`-framework AudioToolbox`.)

- **Clipboard — DONE (all formats, both directions, validated).** Real `appsandbox-clipboard.c` (ch5) +
  `appsandbox-clipboard-reader.c` (ch6) wired to `asb_transport`; host engine in `asb_viewer`
  (`clip_writer_thread`/`clip_reader_thread`) mirrors `vm_clipboard.c` (FORMAT_LIST→DATA_REQ→DATA_RESP).
  Text, RTF, HTML (CF_HTML wrapper), images (CF_DIB↔PNG), files/folders (CF_HDROP file-transfer).
  Two host fixes: NSPasteboard writes on the main thread; run the viewer as the GUI user (not root).
  Details in `interactive-vm-runbook.md`.

**Interactive feature set COMPLETE: display, cursor, input, audio, clipboard — all over ivshmem from the
real product binaries.** Remaining = productionization: agent control channel ch1/ch7 (task #18, needs a
Mac host control consumer), host‑side production transport in `src/backend_mac` (task #21, replaces the
`asb_viewer` test host), Mac QEMU backend (Workstream D), disk‑builder provisioning bake (Workstream C),
acceptance (G).

---

## FROM‑SCRATCH VM WORKING (2026‑06‑17) — build + boot + provision a Windows VM entirely from macOS
A brand‑new `osType="Windows"` VM is now created from a stock ISO, end to end, via the product API — our
own code does everything (no DISM, no hacks). Validated: build → boot (QEMU+HVF, testMode, headless) →
drivers install with per‑driver certs → **agent online via main's UNMODIFIED FirstLogon (~7 min)** → SSH
key login (`sshState 4`) → display proven over ivshmem (viewer DISPLAY ESTABLISHED).

- **iso‑patch‑mac `build-windows` — DONE (Workstream C).** New subcommand in
  `tools/iso-patch-mac/iso-patch-mac.m`: mounts the ISO (`hdiutil`), our `engine/` NTFS writer applies
  `install.wim` + stages the payload manifest, the **shared** provisioning generator bakes the on‑disk
  scripts, in one unprivileged step (stdout `STATUS:`/`PROGRESS:`/`LOG:`/`DONE:`/`ERROR:`). `--ssh-msi`
  arg; manifest drv[] includes `asb_ivshmem.cer`.
- **Unified provisioning generator — DONE.** `tools/provision/win_provision.{h,c}` (NEW): one
  platform‑neutral generator compiled into BOTH the Windows backend and the iso‑patch‑mac Xcode target,
  emitting `unattend.xml` + `setup.cmd` + `SetupComplete.cmd`. **Scripts = main + only the ivshmem
  driver — NO is‑mac/is‑pc fork** (per user directive). Installing the ivshmem driver on a Win→Win VM is
  a harmless no‑op. SetupComplete order: testsigning → **ivshmem FIRST** (certutil `asb_ivshmem.cer` to
  Root+TrustedPublisher, then `devcon install asb_ivshmem.inf "PCI\VEN_1AF4&DEV_1110&SUBSYS_11001AF4&REV_01"`)
  → VDD (cert + `devcon install Root\AppSandboxVDD`) → VAD (devcon, no cert — WHQL‑signed) → OpenSSH.
- **Per‑driver cert trust — DONE.** 3 drivers, 3 certs (`[[driver-signing-certs]]`): VDD=`WDKTestCert
  user`, VAD=`Microsoft Windows Hardware Compatibility Publisher` (already trusted), ivshmem=`AppSandbox
  Test Cert`. The ivshmem `.cer` was extracted from its `.cat` and is now staged + certutil‑trusted before
  install. (This was the SetupComplete‑hang root cause: untrusted publisher in non‑interactive SYSTEM.)
- **NTFS‑writer ACL ROOT‑CAUSE FIX — DONE (`[[ntfs-writer-dir-acl-inheritance]]`).** The agent‑not‑coming‑
  online bug was NOT timing/NIC/scripts: our writer (`engine/win_disk.c mkdir_p`) stamped staged dirs with
  a **PROTECTED, non‑inheritable** SD, so runtime `setup.log` got Admins read‑only → FirstLogon's
  `setup.cmd` couldn't write its log → silent failure → agent never installed. Fix = inheritable dir SD
  (`asb_build_dir_sd`/`g_dir_sid`, SDDL `D:(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICIIO;FA;;;CO)(A;OICI;0x1200a9;;;BU)`).
  VERIFIED in‑guest: new dir ACL is `(OI)(CI)(F)` for Admins; setup.cmd started+finished markers present.
- **Mac QEMU backend networking — DONE (interim, Workstream D partial).** `src/backend_mac/qemu_vm.m`:
  `-netdev vmnet-shared` (full host↔guest routing). Root daemon → NSTask, no prompt; unprivileged →
  AEWP elevation prompt on VM launch (`launchElevated:`/`watchElevatedExit`), **no graceful fallback**
  (per user). Monitor over loopback TCP (`-monitor tcp:127.0.0.1:<port>`). `com.apple.vm.networking` is
  a managed entitlement (Apple‑granted) — using root in the interim until the user requests the profile.
- **Payload staging via Xcode post‑build — DONE.** Run Script phase "Embed agent_win" copies
  `tools/agent_win/` (EXEs + VDD/ivshmem/VAD driver pkgs + certs) + `devcon` into the app bundle
  `Resources/agent_win`. `windows_payload_directory()` = the single seam for the future JIT pull. 13
  engine `.c` + `win_provision.c` added to the iso‑patch‑mac target.
- **Mac core + headless API Windows branch — DONE.** `asb_core_mac.m` `start_windows_build_flow` +
  `asb_mac_vm_create` Windows branch (preauthorize skipped for Windows); `headless.m` POST /v1/vms
  accepts `osType=Windows` (requires `imagePath`). `iso_patch_mac.m`
  `buildWindowsDiskWithISO:…` + `ensureOpenSSHMsiCached` (downloads OpenSSH‑ARM64 MSI).

---

## 2026‑06‑18 session — vendoring, licensing, quit/race fixes, and the asb‑vmm spike

### QEMU fork = single source of truth (`[[qemu-fork-source-of-truth]]`)
- Forked QEMU to **github.com/jamesstringer90/asb-qemu**, branch **`asb-ivshmem-11.0.1`** = upstream
  tag `v11.0.1` + the **3 ivshmem commits** (replaces the old tarball‑download + local patch files).
  This fork **is** our GPL corresponding‑source. 11.0.1 is the latest stable (11.0.0 = Apr 2026 + the
  .1 patch) so the pin is newest‑stable AND validated — never track master/HEAD.
- `vendor/qemu-ivshmem/build-qemu.sh` now **shallow‑clones the fork@branch** (no tarball, no patch
  step) and configures a **raw‑hypervisor‑only** build: `--enable-hvf --enable-vmnet` + a long list of
  `--disable-*` (pixman, slirp, vnc, gtk/sdl/cocoa, spice, gnutls/nettle/gcrypt, capstone, libssh,
  libusb, curl, zstd/bzip2/lzo/snappy, png, tpm, coreaudio, docs, tools…). Result: the shipped binary's
  non‑system deps drop from ~13 Homebrew dylibs to **glib only** (~34 MB binary). pixman is safely
  disabled — no graphics device (`-display none`; the VDD is the display over ivshmem ch2).

### QEMU + EDK2 vendored and EMBEDDED into the app bundle — Workstream D vendoring DONE (`[[qemu-vendored-embed]]`)
- `vendor/qemu-ivshmem/stage.sh` gathers the binary + its dylib closure into **`dist/`**, rewrites
  install‑names to `@loader_path/../lib`, **codesigns the binary with `qemu.entitlements`
  (`com.apple.security.hypervisor`)** — without it HVF is denied and no VM boots (`[[qemu-hvf-entitlement]]`;
  the old `/tmp/qbuild` binary happened to have it, the fresh fork build did not), and **gzips the EDK2
  firmware** (128 MB → 1.6 MB).
- New Xcode **"Embed QEMU" build phase** (runs `embed-into-bundle.sh` on every build) copies `dist/` →
  `AppSandbox.app/Contents/Resources/qemu/` (`bin/qemu-system-aarch64`, `lib/*.dylib`,
  `share/qemu/edk2-*.fd.gz`). Verified: embedded qemu runs **from the bundle** at 11.0.1, keeps the
  `@loader_path` deps + the HVF entitlement after the full app codesign, and does NOT inherit the app's
  0.1.3 version (it has no Info.plist; `stamp-version.sh` only touches the app's plist).
- `src/backend_mac/qemu_vm.m`: resolves **bundle‑first** (`Resources/qemu/...`), dev‑fallback = walk up
  to `vendor/qemu-ivshmem/dist` (**dropped the fragile `/tmp/qbuild` + `iceman/Vendor/qemu` paths**).
  Firmware is **gunzipped at VM‑create** via the base‑system `/usr/bin/gunzip` (zero dev‑tool deps on
  the user's Mac): code.fd → a shared `<appsupport>/firmware/` cache (once), vars.fd → per‑VM. NIC uses
  `romfile=` (drops `efi-virtio.rom`). Whole app ≈ **44 MB**, fully self‑contained at runtime.

### Signing + notarization — PROVEN end‑to‑end (`[[qemu-hvf-entitlement]]`)
- The embedded qemu + dylibs live under `Resources/`, which Xcode does NOT auto‑sign, and Apple
  notarization is recursive — every nested Mach‑O must be Developer ID + hardened runtime + a secure
  timestamp or notarytool rejects the whole `.app`. The Xcode build **script‑phase sandbox can't reach
  the signing keychain** (`codesign: no identity found`), so the split is: `embed-into-bundle.sh`
  ad‑hoc signs (valid dev build, qemu keeps the HVF entitlement); `tools/sign/make-release-mac.sh`
  then re‑signs qemu + its dylibs with **Developer ID + `--options runtime --timestamp`** (qemu also
  `--entitlements qemu.entitlements`) and **re‑seals the app** (`--preserve-metadata=identifier,
  entitlements,flags`), then verify → `notarytool submit --wait` → staple.
- **VERIFIED:** a full `make-release-mac.sh` produced an app where EVERY nested Mach‑O (app, framework,
  agent, clipboard, iso‑patch, **qemu + its 6 dylibs**) is Developer ID + hardened runtime; submitted
  to Apple → **status Accepted**, stapled, `spctl` → "Notarized Developer ID". qemu did NOT need
  `com.apple.security.cs.allow-jit` (HVF‑only, no TCG). Identity "App Sandbox LLC (6G72AZC38T)", notary
  profile `appsandbox-notary`. The git‑ignored signing files (`src/app_mac/xcconfig/LocalUser.xcconfig`,
  `tools/sign/mac-signing.local.json`) were copied into the canonical repo.

### Still open after 2026‑06‑18 (validation gap + new track)
- **NOT YET runtime‑validated:** no VM has been booted on the new self‑contained bundle (minimal fork
  qemu + gunzip'd firmware). `smoke-boot.sh` and a full create→boot are unrun; the guest‑shutdown race
  fix is built but not re‑exercised. **Highest‑value next step.**
- Plus the pre‑existing v0 items (REMAINING WORK below): R5 (Windows‑side parity), task #21 (host‑side
  production transport + agent ch1/ch7 consumers), Workstream E stop‑path + `display_ready` latch, R10
  acceptance automation; and the separate **asb‑vmm** track (M0 done → oracle → M1..M6, tasks #49‑56).
- **Guest‑agent clean shutdown (open Windows task; Win→Win AND Win→Mac):** the agent + its 4 channel
  helpers don't exit on a Windows shutdown → the guest's ACPI power‑off times out. Fix = honor
  `SERVICE_CONTROL_SHUTDOWN` / `WM_QUERYENDSESSION`. (Was tracked in memory only; now in the plan +
  `TODO/WINDOWS_AGENT_BRIEF.md`.)

### LGPL/GPL compliance — ship Homebrew's prebuilt glib/gettext/pcre2 (decided NOT to build our own)
- Decision: ship Homebrew's precompiled dylibs (glib 2.88.1 + Homebrew's `hardcoded-paths.diff`,
  gettext 1.0 = `libintl`, pcre2 10.47), dynamically linked. A build‑from‑source `build-deps.sh` was
  tried then removed — **libffi 3.4.6 won't assemble on current clang** (CFI directive bug); and macOS
  ships no system glib, so glib must come from build‑or‑prebuilt either way.
- **Compliance kit in the repo** (`vendor/qemu-ivshmem/`): `fetch-sources.sh` mirrors the **LGPL
  corresponding source** (glib tarball + the Homebrew patch + the per‑keg formula; gettext tarball),
  all **sha256‑verified**, into `source-mirror/`; `LICENSES/` (LGPL‑2.1, GPL‑2.0, pcre2 BSD, EDK2
  BSD‑2‑Clause‑Patent, `NOTICE.md`); `WRITTEN_OFFER.txt` (TODO: fill the contact email); `stage.sh`
  writes `dist/VERSIONS.txt`. `embed-into-bundle.sh` copies LICENSES + offer + VERSIONS **into the
  bundle** (NOT the 37 MB source tarballs — those stay in the repo, referenced by the offer). Two
  gotchas covered: gettext/`libintl` IS shipped (LGPL), and Homebrew DOES patch glib (the patch is part
  of the corresponding source).

### Quit behavior — VMs die with the app, matching macOS guests
- The app had **no running‑VM quit guard**: a VZ guest dies when its in‑process object is released, but
  QEMU (an external child) orphaned and kept the instance lock. Fix (user chose "match macOS exactly"):
  `FD_CLOEXEC` on the instance‑lock fd (`headless.m`, so spawned children don't inherit it);
  `asb_mac_cleanup` now **stops QEMU + closes its transport**; `AppDelegate applicationWillTerminate:`
  calls `asb_mac_cleanup`. No prompt, no graceful ACPI — QEMU is killed like the VZ object is released
  (accepted NTFS plug‑pull trade‑off).

### ivshmem guest‑shutdown crash — FIXED (`[[ivshmem-shutdown-race-regionforchannel]]`)
- Shutting Windows down from inside the guest crashed the app (EXC_BAD_ACCESS in `regionForChannel:`):
  QEMU exit → `[AsbIvshmemTransport close]` munmaps the BAR while the agent reconnect loop derefs `_dir`
  via the **unlocked** entry to `connectChannel:`; `close()` never nil'd `_dir`. Same class as the
  earlier #47 fix, which had missed this deref. Fix: `connectChannel:` resolves `regionForChannel:`
  **under `_lock`** behind the `_closed/_bar/_dir` guard, and `close()` nils `_dir`. Invariant: every
  `_dir`/`_bar` deref in the transport must be under `_lock`.

---

## Ground truth (read in full — still accurate)
- **Guest binaries + transport seam** — 8 identical `socket(AF_HYPERV,SOCK_STREAM,1)` +
  `SOCKADDR_HV{VmId, ServiceId=a5b0cafe-000N}` sites: `agent.c` (ch1 listen ~L2070, ch7 ssh ~L2244),
  `appsandbox-input.c` (ch3), `-audio.c` (ch4), `-clipboard.c` (ch5), `-clipboard-reader.c` (ch6),
  `vdd.cpp` `VddNetworkThread` (ch2 `FRAME_SERVICE_GUID`), `p9copy.c` (ch50001). Protocols above the
  socket are platform‑neutral (`hello`/heartbeat/`<seq>:cmd`; magics `ASIN/ASA1/ACLP/CLDY/IRDY/
  ASFR/ASCR`). **These 8 sites are what the ivshmem transport abstraction replaces on Mac.**
- **Host↔agent control handshake** (`vm_agent.c`) + Mac twin (`vm_agent_mac.m`) — identical flow;
  Mac connector's only platform code is `connectOnce`. Host service GUID via
  `hcs_service_guid(os_type, channel, &g)` — the **os_type‑keyed seam** for per‑host transport.
- **Provisioning recipe** (`disk_util.c`): staged manifest (6 EXEs → `%SystemRoot%\AppSandbox\`;
  `drivers\` = `AppSandboxVDD.{dll,inf,cat,cer}` + `devcon.exe` + `AppSandboxVAD.{sys,inf,cat,cer}`;
  OpenSSH MSI), `generate_unattend_vhdx` (specialize: `bcdedit /set testsigning on` when test_mode +
  ARM64 LabConfig bypass; oobeSystem AutoLogon/FirstLogonCommand → `setup.cmd`), `setup.cmd`
  (`appsandbox-agent.exe --install`), `SetupComplete.cmd` (certutil Root+TrustedPublisher;
  `devcon install` VDD then VAD; OpenSSH msiexec + `net start sshd`). **← our ivshmem driver is staged
  + installed here the same way (runbook PRODUCTION DELTAS #1‑2).**
- **Mac core** (`asb_core_mac.m`): lifecycle + `start_agent_for`/`start_ssh_proxy_for`/
  `start_clipboard_for`; install → `IsoPatchMac stageAgentIntoDiskAtURL:`. Now ALSO carries the
  `os_type=="Windows"` create/start branch (`start_windows_build_flow` → QEMU+HVF via `qemu_vm.m` +
  the `IddDisplayWindow` host consumer); the VZ path is kept for macOS guests.
- **Mac VM creation** (`vz_vm.m`): VZ `VZMacOSBootLoader` (cannot boot Windows) → Windows‑on‑Mac needs
  a **new QEMU+HVF backend** (Workstream D).
- **iso-patch-mac R&D:** `engine/` from‑scratch NTFS writer boots Win11 ARM64 to desktop, chkdsk‑clean,
  multithreaded; baked unattend reaches desktop hands‑free.
- **README:** Test Mode = test‑signing **inside the guest**; host boot config untouched.

---

## Transport design — ivshmem shared‑memory channels (replaces the vsock socket‑swap design)
Shared `asb_transport.{h,c}` linked into every guest binary; replaces the 8 inlined AF_HYPERV sites.
- **Host selection (auto, default‑safe; per "don't assume Hyper‑V machine type"):**
  1. Explicit override (`HKLM\SOFTWARE\AppSandbox\Transport` / env — our Mac builder sets `ivshmem`).
  2. Else positively detect **ivshmem** (our driver's device interface present →
     `SetupDiGetClassDevs(GUID_DEVINTERFACE_ASB_IVSHMEM)`).
  3. Else **default AF_HYPERV** (HCS/hvsocket) — PC behavior byte‑identical. Never positively detect
     "Hyper‑V".
- **Layout over the BAR — default 128 MiB (to design/build — Workstream B):** a control/discovery header +
  per‑channel **SPSC ring buffers** for the low‑rate stream channels (ch1 agent, ch3 input, ch5/6
  clipboard, ch7 ssh, ch4 audio) so the agent's `recv/send`/listen‑accept model maps onto ring
  read/write; + a **double‑buffered frame region** for the VDD (ch2) where it writes BGRA frames the
  host reads (its existing `ASFR/ASCR` framing, just into shared memory instead of a socket).
  Notification = polling (gave ~3 µs round‑trip; a doorbell can be added later). The protocols ABOVE
  the transport are untouched.
- **Mac side:** the Mac app `mmap`s the ivshmem backing file and speaks the same ring/frame layout;
  `vm_agent_mac`/`vm_ssh_proxy_mac`/`vm_clipboard_mac` get an `AsbSocketConnecting`‑style impl whose
  read/write hit the rings instead of `[VZVirtioSocketDevice connectToPort:]`.

---

## Workstreams (all required for acceptance)
- **A. Dev VM build box — ✅ DONE.** Test‑signing + Secure‑Boot‑off; VS2022 + Windows SDK 26100 + WDK
  26100.6584 installed (D:); bridged networking; driven over SSH. (runbook Phase 2.)
- **B. Guest transport over ivshmem + host auto‑detect — ✅ DONE (guest side).** `asb_transport` is
  built into ALL guest binaries: `agent.c` (ch1 control + ch7 SSH), the 4 channel EXEs
  (`appsandbox-input` ch3 / `-audio` ch4 / `-clipboard` ch5 / `-clipboard-reader` ch6), `p9copy.c`
  (ch9P connect‑out, PC AF_HYPERV path kept), and `vdd.cpp` (ch2 frame + cursor). Host auto‑detect =
  `asb_transport_init` (override → positive ivshmem detect → default AF_HYPERV). Protocols untouched;
  Win→Win byte‑identical. Full detail in `TODO/WINDOWS_AGENT_BRIEF.md`. (R5 = the Windows BUILD/
  provisioning parity for this guest work is what remains — see REMAINING WORK.)
- **C. Disk builder full provisioning bake — ✅ DONE.** `engine/` stages the exact `disk_util.c`
  manifest **plus `asb_ivshmem.{sys,inf,cat,cer}`** + on‑disk scripts from the shared `win_provision.c`
  generator. `build-windows` subcommand emits the stdout protocol. (See FROM‑SCRATCH section.)
- **D. Mac QEMU Windows backend + ivshmem — ⏳ PARTIAL.** `qemu_vm.m` boots Windows on QEMU+HVF with
  `vmnet-shared` networking + loopback monitor; ivshmem device attaches; transport substrate validated.
  **Remaining:** productionize the host‑side ring/frame consumers in `src/backend_mac` (replace the
  `asb_viewer` test host) — see REMAINING WORK R1 + agent ch1/ch7 host consumers (R3, task #18/#21).
  **Vendoring DONE (2026‑06‑18):** the patched QEMU + EDK2 are built from the fork, staged to
  `vendor/qemu-ivshmem/dist`, codesigned with the HVF entitlement, and embedded into the bundle by the
  Xcode "Embed QEMU" phase — fully self‑contained at runtime (see the 2026‑06‑18 section).
- **E. Mac core `os_type=="Windows"` branch — ✅ DONE (build/start).** create→build+stage via extended
  `IsoPatchMac`; start→`qemu_vm.m`; `gpuMode` always `gpu_none`. **Remaining:** stop→agent `shutdown`
  then QEMU `system_powerdown` (needs the host control consumer); `display_ready` latch wiring.
- **F. Test‑mode/secure‑boot create option — ⏳ CLI DONE, GUI PENDING.** `testMode` bakes testsigning +
  QEMU guest‑Secure‑Boot‑off; exposed in headless API. **Remaining:** web GUI still BLOCKS Windows on
  Mac (`web/app.js` `unavailable=['Windows','Linux']`) + `.iso` picker (`ui.m handleBrowseImage` only
  allows `.ipsw`) — see REMAINING WORK R2.
- **G. Acceptance — ⏳ PENDING.** Manually validated end‑to‑end via the API (build/boot/online/ssh).
  **Remaining:** `run_all.py` Windows spec green automated; `display_ready` latch (needs R1); graceful
  shutdown path; Windows‑side parity so a release‑signed build reproduces it.

---

## REMAINING WORK (2026‑06‑17 — what's left to ship v0)
Numbered in priority order — what unblocks a usable product from the GUI comes first. Nothing here is
committed yet (`[[no-commits-until-v0]]` — all in the working tree).

> **Progress (2026‑06‑17, batch):** ✅ **R2** (Windows creatable from the Mac GUI: app.js gating flipped +
> `.iso` picker in ui.m), ✅ **R3** (ch1/ch7 confirmed already wired for Windows; dead `vm_ssh_proxy_ivshmem`
> removed; clipboard rolled into R1/P3), ✅ **R4** (guest NIC → virtio‑net‑pci + NetKVM/virtio‑win + per‑VM MAC; usb‑net/RNDIS was tried but Win11 ARM64 binds it as a COM port, so reverted), ✅ **R7**
> (`appsandbox-displays.exe` + VAD trio staged from the 0.1.3 release into `tools/agent_win`), ✅ **R8**
> (delete‑crash fixed: Windows teardown branch + `g_qemu_refs`/`g_transport_refs` shift), ✅ **R6** (ssh ch7
> hardening: the single‑threaded `asb_ivshmem_pump_main` deadlocked on a sustained full‑duplex burst —
> made the pump's socketpair end non‑blocking + carry partial writes with select() backpressure; host‑side
> only, no guest rebuild), ✅ **R1** (host display consumer: `src/backend_mac/idd_display.{h,m}` —
> `IddDisplayWindow` connects ch2 VDD frames + cursor, ch3 input, ch4 audio, AND ch5/ch6 clipboard (both directions) over the ivshmem transport into a real
> AppKit window; wired into `handle_qemu_state_change` Running/Stopped, `asb_mac_open_display`, and the
> delete teardown; added to the Xcode project). **`** BUILD SUCCEEDED **` verified (whole tree, Debug).**
> Still source‑only on the guest side: R5 (Windows parity), R6's note (guest unchanged). Remaining items
> below: R5 (Windows parity), R9 (JIT pull), R10 (acceptance), R11 (robust slot‑reclaim), + a runtime QA pass.
> (Clipboard ch5/ch6 host‑side — the old "P3" — is DONE in `IddDisplayWindow`, above.)

**R1 — Wire host display consumer (productionize VDD ch2 in the app).** Today the guest VDD LISTENS on
ch2 and only the standalone `asb_viewer` test tool connects to it. The AppSandbox daemon does NOT
connect → no guest desktop in the product window. Build a host display component in `src/backend_mac`
that, on Windows‑VM start, `asb_connect`s to ch2 (kicking the VDD into emitting), parses the VDD wire
frames + cursor (mirror `tools/transport/test/asb_viewer.m`), and renders into a real AppKit window.
This is also what latches `display_ready`. Distinct from ch1 (agent, wired) and ch7 (SSH, wired).

**R2 — GUI enablement (create Windows VMs from the Mac UI).** Two blockers: (1) `web/app.js` (~L84)
`var unavailable = hostBridge.isMac ? ['Windows','Linux'] : ['macOS']` + forces `osSelect.value='macOS'`
→ flip to allow Windows on Mac; (2) `src/app_mac/ui.m handleBrowseImage` NSOpenPanel
`allowedContentTypes` only `.ipsw` → add `.iso`. Then verify the create call + the unprivileged‑GUI
elevation prompt (AEWP vmnet) fires on launch.

**R3 — Agent control host consumer (ch1/ch7) — task #18/#21.** The Mac host control consumer that reads
the agent ring (`hello`/heartbeat) and the SSH proxy are wired for the dev path; productionize them in
`src/backend_mac` (replace the `asb_viewer`/standalone test hosts) so create/start/stop/online/ssh all
run from the daemon. Stop path = agent `shutdown` then QEMU `system_powerdown` (`[[shut-down-vms-safely]]`).

**R4 — Guest NIC driver — ✅ DONE.** NIC = **`virtio-net-pci`** + the **NetKVM** (virtio-win, WHQL-signed)
guest driver: downloaded at VM-create (`asb_core_mac.m` ~L947, the ~789 MB virtio-win pull, cached) and
staged + installed via the provisioning manifest (`tools/iso-patch-mac/iso-patch-mac.m` ~L1572:
`netkvm.{inf,sys,cat}` + `netkvmp.exe` + `virtio-win_license.txt` → `\Windows\AppSandbox\drivers\`). The
NIC device is set in `qemu_vm.m` ~L246 (`virtio-net-pci,netdev=net0,...`). **usb-net/RNDIS was tried and
reverted** — Win11 ARM64 binds it as a COM port, not a NIC (see the comment at `qemu_vm.m` ~L241-244).
NetKVM is the one guest driver beyond the QEMU+EDK2 dep rule. NOT required for boot/agent/ssh (those ride
ivshmem); required for guest internet + full parity.

**R5 — Windows‑side parity for the unified generator (source‑only, build on the Win dev VM).**
`disk_util.c` must delegate `generate_vhdx_*` to the shared `win_provision.c`; add
`asb_ivshmem.{inf,sys,cat,cer}` to `generate_vhdx_manifest`; compile `win_provision.c` into
`AppSandbox.vcxproj`; have the `tools/ivshmem` vcxproj emit into `release/resources/drivers`. Keeps PC
(Win→Win) byte‑identical and proves a release‑signed build reproduces the Mac result.

**R6 — Harden `ssh_proxy` (ch7).** Drops on larger/certain output — relay needs proper
backpressure/framing.

**R7 — Missing payload files (existence‑gated, build on Win dev VM).** Add `appsandbox-displays.exe` +
`AppSandboxVAD.cer` to `tools/agent_win`.

**R8 — Daemon crash on Windows‑VM delete.** Deleting a Windows VM via the API crashes the daemon.
Current workaround: stop daemon, edit `vms.cfg` + `rm` the dir, restart. Needs a real fix.

**R9 — JIT payload pull (future, not v0).** Replace the committed `tools/agent_win` with a just‑in‑time
download of the Windows release payload at VM‑build time. `windows_payload_directory()` is the single
seam. User: "not ready for that yet."

**R10 — Acceptance automation (Workstream G).** `run_all.py` Windows spec green end‑to‑end (depends on
R1 for `display_ready` + the graceful‑shutdown path).

**R11 — Robust ivshmem slot‑reclaim across agent/VDD restart (OPEN).** The baseline transport does NOT
reclaim a slot a dead guest peer left `ESTABLISHED` (agent restart, VDD restart, or a host crash): the
host connector (`asb_ivshmem_transport.m connectChannel:`) only claims `FREE`/`CLOSED` slots and nothing
detects peer death, so the host pump for a dead peer never exits → a restarted agent/VDD can fail to
reconnect until reboot (single‑slot channels dead; multi‑slot ssh leaks slots → "vsock connect failed").
A heartbeat + CAS‑accept + stale‑exit fix WAS implemented (guest `guest_hb` + host pump watch) and then
**REVERTED**: it killed ch1 because the agent's control channel idles **without** going through
`asb_recv`/`asb_poll`, so it never beat and the host pump tore it down. A correct fix needs the agent's
idle ch1 wait to beat (or a liveness signal independent of transport traffic), plus host‑crash reclaim by
selection only (never adopt‑on‑loss — that double‑owned a slot and crashed the host). Current state =
stable baseline, only the SPSC‑ring acquire‑barrier ssh fix kept; **no auto‑reclaim.**

---

## CONFIRMED (was open)
- **Transport — RESOLVED: ivshmem shared memory** (built + validated above). The old open question
  (virtio‑serial vs vsock vs TCP) and the "rebuild QEMU for vhost‑user‑vsock" plan are **superseded**
  (see Superseded below).
- **Performance — RESOLVED:** cached ivshmem ~28 GB/s single‑thread, ~3 µs RTT, coherent. The display
  (only memcpy‑class channel) rides the ivshmem frame region — the VDD is **kept** (frames into shared
  memory, not the QEMU native display, which is temporary).
- **Test Mode = Secure Boot OFF in the VM firmware** (`hcs_vm.c`) + `bcdedit testsigning on`. QEMU boots
  AAVMF with guest Secure Boot off when testMode (Iceman recipe already does).
- **`hcs_service_guid(os_type, port, &out)`:** Windows → `a5b0cafe-<port>-4000-8000-000000000001`;
  Linux → `<port>-FACB-11E6-BD58-64006A7986D3`. PC keeps AF_HYPERV by default.

## OPEN QUESTIONS (remaining)
1. **Ring/frame layout details** over the 128 MiB BAR (default; sizes, double‑buffer vs triple,
   doorbell vs poll, backpressure) — design in Workstream B; the primitive + perf are known. Sizing:
   4K frame = ~32 MiB (double‑buf ~63), channel rings ~2–16 MiB (9P/ssh/clipboard dominate); 128 MiB
   fits 4K double‑buffer + channels, 256 MiB for 4K triple or multi‑monitor, 64 MiB = 1080p‑only.
2. Mac display for the Windows QEMU guest during bring‑up (QEMU cocoa window) vs final (VDD frames via
   ivshmem to a Mac view). Only `display_ready` is required for the test; parity needs the real view.
3. Production signing pipeline: stable AppSandbox cert for `asb_ivshmem.{sys,cat}` (same as VDD/VAD),
   baked + installed via `SetupComplete.cmd` — runbook PRODUCTION DELTAS #1‑2.

---

## Acceptance path — mapped from read code (transport = ivshmem on Mac)
- **online (ch1):** guest agent on the ivshmem agent‑ring; `VmAgentMac` reads it → `hello`/heartbeat.
- **display_ready:** agent reports `idd_status:ok` (VDD installed+running); core latches
  `running && agent_online && idd_ready`.
- **ssh key + login (ch7):** OpenSSH installed + key deployed (agent `ssh_deploy_key`); guest agent
  ch7 proxy (ivshmem ring ↔ `127.0.0.1:22`); host `VmSshProxyMac` (TCP loopback ↔ ring); `ssh -i key`.
- **graceful shutdown:** core → agent `shutdown` → `InitiateSystemShutdownExW`; QEMU fallback
  `system_powerdown`.
- **force‑stop/delete:** QEMU terminate + VM‑dir delete.
- **Connector swap is the ONLY Mac‑side change:** `VmAgentMac`/`VmSshProxyMac`/`VmClipboardMac` →
  `AsbSocketConnecting` with an ivshmem‑ring impl.

---

## SUPERSEDED (kept for history — do NOT implement)
- **vhost‑user‑vsock** (earlier 2026‑06‑16 decision): rebuild QEMU + `vhost-device-vsock` daemon.
  Dropped — vhost‑user needs Linux `eventfd`/`memfd` (absent on macOS); not viable. ivshmem replaces it.
- **virtio‑serial / TCP transport** (earlier "viosock not available" finding): virtio‑serial works but
  is a ReadFile/WriteFile device model (poor fit for the listen/accept agents) and tuned for control,
  not bulk; TCP uses networking (user rejected). ivshmem (shared memory) is faster and non‑network.

## Reading log
- Read fully: agent.c, appsandbox-{input,audio,clipboard,clipboard-reader,displays}.c, p9copy.c/.h,
  vdd.cpp/.h, vm_agent.c/.h, disk_util.c/.h, vm_agent_mac.m, vz_vm.m, asb_core_mac.m, iso_patch_mac.h/.m,
  hcs_vm.h + hcs_vm.c, vm_ssh_proxy_mac.m, README.md, tests/README.md, vm_lifecycle.py.
- Still to read (at their impl step): vm_clipboard_mac.m, vm_display_idd.c, vm_clipboard.c, vm_display.c,
  vm_ssh_proxy.c, vz_display.m, vz_network.m, tools/vad/*, vmms_cert.c, asb_core.c (lifecycle),
  asb_types.h, asb.py/helpers.py/run_all.py, web/app.js, the .vcxproj build files.
