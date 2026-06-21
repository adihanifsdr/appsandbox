# Windows VMs on macOS — AppSandbox Integration Plan

> Execution plan for folding the validated ivshmem / QEMU+HVF / iso-patch work into the
> AppSandbox product so a user can create, install, and use a Windows VM from the macOS app
> exactly as on Windows, **minus GPU-PV**. This is the productionization of the workstreams in
> `windows-on-mac-plan.md` (the R&D living doc); read that for the proven-out substrate. Every
> file/line reference below is from the current tree (verified, not assumed).

## Status (current — verified against the tree)

**P0–P5 are implemented; P6 (UI/API + vendoring) and P7 (acceptance) remain.** The Windows-on-Mac
create→boot→online→display→ssh path runs end to end on the dev host (VM `MyAppSandbox`, driven via
the `--headless` daemon + `tools/headless-api/asb.py`): agent online over ivshmem ch1, VDD display
over ch2, ssh-over-ivshmem on ch7. §§2–9 below are the original plan; the cited files now hold the
implementation:
- **Transport / VM (§4/§6):** `src/backend_mac/asb_ivshmem_transport.m` (host ring↔fd pump),
  `qemu_vm.{h,m}` (QEMU+HVF lifecycle). NIC = **`virtio-net-pci` + NetKVM (virtio-win)** over
  `vmnet-shared` — usb-net/RNDIS was tried and rejected (binds as a COM port on Win11 ARM64), so the
  "R4 usb-net" note elsewhere is superseded.
- **Display (§5):** `idd_display.{h,m}` (`IddDisplayWindow`, ch2 frames + ch3 input); wired in
  `asb_core_mac.m` (`open_idd_display_for`, `handle_qemu_state_change`).
- **Core (§3):** `asb_core_mac.m` `os_type=="Windows"` branch (QemuVm + per-VM ivshmem transport,
  `gpu_mode` forced GPU_NONE). `vm_agent_mac.m:325` now parses `idd_status:` → `display_ready` — the
  §11 "verified gap" is **CLOSED**.
- **Disk (§7):** `iso_patch_mac.m buildWindowsDiskWithISO:` → `tools/iso-patch-mac/iso-patch-mac.m`
  `build-windows` subcommand (manifest-staged payload incl. `asb_ivshmem.*` + NetKVM).
- **UI/API (§9):** `headless.m` accepts `osType` macOS|Windows; `web/app.js` offers Windows on a Mac host.

**Open gap:** the heartbeat/slot-reclaim "reliability" work was **reverted** (it broke agent ch1 —
the agent's idle control channel doesn't beat). The stable baseline keeps only the SPSC ring
acquire-barrier ssh fix; **robust ivshmem slot reclaim on agent/VDD restart is still unsolved.** Code
is committed to branch `win-on-mac` (the "No commits until v0" note in §1 is historical).

## 1. Goal & definition of done

From the macOS AppSandbox UI: pick **Windows**, configure everything **except GPU** (name, vCPU,
RAM, disk size, network, user/password, SSH on/off + key deploy), click **Create**. The app then,
with no further interaction:

1. Builds a bootable Windows disk on the Mac from the user's `install.wim` (iso-patch engine), with
   an `unattend.xml` and the AppSandbox guest payload (agent + channel EXEs + VDD + ivshmem driver
   + SSH bits) baked in offline.
2. Boots the VM on the vendored patched QEMU+HVF with `ivshmem-plain` (no QEMU display window).
3. Windows finishes specialize/OOBE hands-free, installs the agent service + drivers, the agent
   comes **online over ivshmem**, and (if enabled) the SSH key is deployed by the agent.
4. The user opens the display window and sees the live Windows desktop (VDD frames over ivshmem),
   with mouse/keyboard/clipboard/audio, and can `ssh` to it with no guest network.

**Done = parity with a Windows-hosted AppSandbox VM except GPU.** `gpuMode` is accepted in the UI
but always resolves to `GPU_NONE` on a Mac host; the VDD is kept (it is the display path, and the
future GPU hook).

**Non-negotiable quality bar (per user):**
- PC (Windows→Windows / Windows→Linux) behavior stays **byte-identical**. Nothing in this plan
  touches a PC code path; the Mac branches are additive and `os_type`/host-keyed.
- No temporary / "phase-1" shims that ship. Each step is the production implementation.
- No changelog-style comments; changes are focused; comments state intent, not history.
- No new external dependencies beyond **QEMU + EDK2** (`[[no-external-deps-constraint]]`).
- **Build via Xcode.** ALL macOS code builds through `AppSandbox.xcodeproj` — the only two things
  NOT built by Xcode are the vendored **custom QEMU** and **EDK2** libraries. No ad-hoc clang/
  build-once-ship artifacts (`[[macos-code-builds-via-xcode]]`). New Mac code = Xcode targets/sources
  (e.g. the host transport went into the `AppSandboxCore` framework target; the dev-harness publish
  is the `iso-patch-mac publish-shm` subcommand).
- **Mac launches VMs through QEMU.** AppSandbox-on-Mac drives the vendored QEMU to run guests
  (replacing Virtualization.framework on Mac / HCS on Windows). The VZ path remains only for the
  existing macOS guest; the Windows (and future Linux) guest path is QEMU+HVF via `QemuVm` (§6/P2).
- iso-patch engine stays clean-room (`[[iso-patch-clean-room]]`): no reference-impl names.
- **No commits until v0 works end to end.** All work stays in the working tree (incl.
  `tools/transport/`); the user controls commit timing once the full create→boot→online→display
  path is green.

## 2. Current state — what already exists (verified)

### macOS app + core (VZ, macOS-guest only today)
- **Shared web UI** drives everything: `web/app.js` builds a `createVm` message
  (`app.js:426-438`: `osType, ramMb, hddGb, cpuCores, gpuMode, networkMode, imagePath, …`). On a
  Mac host the OS dropdown is **locked to `macOS`** (`app.js:466-472` comment + validation
  branches). WKWebView bridge: `src/app_mac/ui.m` (`handleCreate` reads `osType` default `macOS`,
  `gpuMode`, etc. → `asb_mac_vm_create(...)`, `ui.m:264-281`).
- **Headless HTTP/CLI** surface mirrors it: `src/app_mac/headless.m` (`validate_create_mac`
  currently **locks `osType` to macOS**, `headless.m:315-371`; `gpuMode` accepted 0–2).
- **Core orchestrator** `src/backend_mac/asb_core_mac.m`:
  - `asb_mac_vm_create` (`:778`) — today **always** the macOS path: fetch/locate IPSW →
    `start_install_flow` (`:748`) → `IsoPatchMac installMacOS…` (VZMacOSInstaller) →
    `finish_install` (`:662`) → `IsoPatchMac stageAgentIntoDiskAtURL:` (APFS mount + dslocal) →
    `autostart_after_install` (`:648`) → `asb_mac_vm_start` (`:903`).
  - `asb_mac_vm_start` (`:903`) → `VzVm loadVmNamed:` → start; state machine in
    `handle_vm_state_change` (`:533`): on **Running** opens `VzDisplayWindow` + `start_agent_for`;
    on **Stopped** tears down.
  - Transport helpers are spun up from the VZ socket device: `start_agent_for` (`:438`),
    `start_ssh_proxy_for` (`:401`), `start_clipboard_for` (`:363`) each pull the
    `VZVirtioSocketDevice` out of `vzvm.machine.socketDevices` and hand it to the helper.
  - SSH keypair already handled host-side: `ensure_appsandbox_ssh_key` (`:120`),
    `asb_mac_ssh_key_path` (`:109`); key is deployed **at runtime by the agent**
    (`agent.deployKeyLine`, `:514`), not baked.
- **VZ VM builder** `src/backend_mac/vz_vm.m` — `VZMacOSBootLoader` (cannot boot Windows);
  this is the module a `QemuVm` peer replaces for Windows guests.
- **Disk tooling** `src/backend_mac/iso_patch_mac.{h,m}` + `tools/iso-patch-mac/iso-patch-mac.m`
  CLI: today only `fetch-ipsw` / `install` (VZMacOSInstaller) / `stage` (APFS mount, dslocal user,
  SSH enable). **No Windows path yet.** Runs as root via AEWP; emits the
  `STATUS:/PROGRESS:/LOG:/DONE:/ERROR:` stdout protocol consumed by `iso_patch_mac.m`.
- **VM bundle layout** `src/backend_mac/vm_dir.m`: `~/Library/Application Support/AppSandbox/VMs/
  <name>/` with `disk.img`, plus (macOS) `aux.img`, `hardware.bin`, `machine-id.bin`; registry
  `…/AppSandbox/vms.cfg` (INI, same format as Windows).

### The transport — guest auto-detects host; only the Mac host side is new work
The **guest binaries already pick the right transport at runtime** — no per-binary config, no
per-host build. `asb_transport_init` (`tools/transport/asb_transport.c:168-184`): registry
`HKLM\…\AppSandbox\Transport` override → else `open_ivshmem()` probes the ivshmem device interface
(`SetupDiGetClassDevs(&GUID_DEVINTERFACE_ASB_IVSHMEM)`, `:144`) → else default **AF_HYPERV**. On a
Mac host the ivshmem device is present, so every guest binary (agent ch1/ch7, input/audio/clipboard
ch3/4/5/6, VDD ch2, p9copy) selects ivshmem; on a Windows host it stays AF_HYPERV, byte-identical.
**This is done** (working-tree code under `tools/transport/`; commits come later — see below).

The only transport work left is **host-side**: today the Mac helpers `VmAgentMac` / `VmSshProxyMac`
/ `VmClipboardMac` get their stream from a `VZVirtioSocketDevice` (`connectToPort:` → `dup(fd)` →
plain `send/recv`; `vm_agent_mac.m:189-230`, `vm_ssh_proxy_mac.m:192-226`). For a Windows guest
there is no vsock device — the Mac app reads/writes the ivshmem rings instead. That host ring
reader is **task #21**; everything above the fd (the `hello`/`heartbeat`/`<seq>:cmd`, `ssh_enable`,
clipboard FORMAT/DATA protocols) is unchanged.

### Validated ivshmem substrate (R&D, from real product binaries) — `windows-on-mac-plan.md`
- Patched QEMU 11.0.1 + HVF with `ivshmem-plain`; our own KMDF driver maps BAR2; `asb_transport`
  channel layer (rings + frame region), coherent + ~28 GB/s.
- Guest binaries already wired to `asb_transport`: `agent.c` (ch1+ch7), input/audio/clipboard/
  clipboard-reader (ch3/4/5/6), `vdd.cpp` (ch2 frame server), `p9copy.c` (ch9P). PC AF_HYPERV
  paths byte-identical (host auto-detect: explicit override → ivshmem device-interface present →
  default AF_HYPERV).
- Host side: `src/backend_mac/vm_ssh_proxy_ivshmem.m` already mirrors `VmSshProxyMac` (TCP↔ch7).
  The display/cursor/input/audio/clipboard/9P host logic exists in the **test viewer**
  (`tools/transport/test/asb_viewer.m`) and must be reimplemented as production `src/backend_mac`
  components (task #21).
- iso-patch engine `tools/iso-patch-mac/engine/win_disk.c` builds a bootable Win11 ARM64 NTFS disk
  from `install.wim`, boots to desktop, chkdsk-clean, with `unattend.xml` baked at
  `\Windows\Panther\unattend.xml`. Clean-room.

### What GPU looks like (so we exclude exactly the right thing)
GPU-PV on Windows = `hcs_apply_gpu` after start (`hcs_vm.c:1390-1464`, gated `gpu_mode != GPU_NONE`)
**plus** 9P shares of the host GPU driver dirs into the guest (`hcs_vm.c:1059-1072`). The Mac QEMU
backend does **neither**: `gpu_mode` is forced to `GPU_NONE`, no GPU modify, no driver share.

## 3. Target create flow on macOS (Windows guest) — mirrors the Windows VHDX-first method

```
UI createVm{osType:"Windows", imagePath:<windows.iso>, …, gpuMode:*}   web/app.js, ui.m/headless.m
  └─ asb_mac_vm_create()  ── os_type=="Windows" branch ──┐         asb_core_mac.m
       (gpu_mode forced GPU_NONE on Mac)                 │
  1. ensure_appsandbox_ssh_key() (if deploy)             │  (already exists)
  2. IsoPatchMac buildWindowsDiskWithName:               │  NEW app API → CLI "build-windows"
       inputs: windows.iso, disk.img path, name, user,   │  (root via AEWP, same stdout protocol)
              password, ram/cpu/disk, sshEnabled,        │
              testMode, pubkey, guest payload dir        │
       a. MOUNT the ISO (hdiutil attach) → read          │  ISO carries sources/install.wim
          sources/install.wim (+ detect default lang)    │
       b. engine win_disk: GPT(ESP+MSR+NTFS) + apply WIM │  tools/iso-patch-mac/engine
          (our WIM reader) + BCD (our patcher) → bootable │  = offline analogue of dism+bcdboot
       c. bake unattend.xml (specialize+oobeSystem only)  │  no windowsPE — OS already applied
          + stage payload via manifest (exact dests §7)  │
       d. unmount ISO                                     │
  3. autostart → asb_mac_vm_start()                      │
       └─ QemuVm loadVmNamed: (Windows)  ────────────────┘  NEW QemuVm peer of VzVm
            patched QEMU+HVF, EDK2 firmware, NVMe disk.img,
            ivshmem-plain + memory-backend-file (the VM bundle's shm),
            -display none, vmnet (NAT) net, no GPU.
  4. State machine (handle_vm_state_change):
       Running → open display (ivshmem frame consumer), start_agent_for over ivshmem
       agent online → install_complete latches, clipboard starts, ssh key deploy fires
  5. open display window → AsbDisplayWindow renders ch2 VDD frames + cursor + input
```

This is the macOS port of the Windows **VHDX-first** create path (`vhdx_create_thread`,
`asb_core.c:1039`): Windows runs `iso-patch.exe --to-vhdx <iso> 1 <gb> --output <vhdx> --stage
<manifest>` (`asb_core.c:1102`), which inits a GPT VHDX, `dism /Apply-Image` the WIM, `bcdboot`s the
ESP, and stages the manifest offline (`iso-patch.c do_to_vhdx`, `:708`). The Mac engine
(`win_disk`) does the identical job with its own WIM reader + NTFS writer + BCD patcher (no
dism/bcdboot — those are Windows-only) and the same manifest. Because the OS is applied **offline**,
the unattend has no windowsPE pass — only specialize + oobeSystem (exactly `generate_unattend_vhdx`,
`disk_util.c:1368`): ComputerName, BypassNRO, `bcdedit recoveryenabled/bootstatuspolicy`,
`testsigning` (testMode), ARM64 LabConfig TPM/SecureBoot bypass, then OOBE-skip + local-admin +
AutoLogon(LogonCount 1) + FirstLogonCommand → `C:\Windows\AppSandbox\setup.cmd`. First boot goes
straight into specialize → OOBE-skip → first-logon (agent `--install`) → SetupComplete (driver
installs) → desktop. `install_complete` still latches on **first agent contact**
(`asb_core_mac.m:467`), same signal as today.

## 4. Host-side transport (task #21) — the only transport work left

The guest auto-detects (§2). On the Mac host, the channel helpers `VmAgentMac` / `VmSshProxyMac` /
`VmClipboardMac` need a connected stream for a Windows guest from the ivshmem rings instead of from
a `VZVirtioSocketDevice`. Keep their loops and protocol code untouched by giving them a stream the
same shape they have today (a blocking fd):

- A small host transport that, per channel, claims an `asb_transport` slot in the bar-mapped
  directory and exposes it as an fd (e.g. `socketpair` + a pump thread bridging the ring) so callers
  keep plain `send/recv`/`select()`. Reconnect semantics already match recv→0 / poll<0
  (`[[ivshmem-reconnect-semantics]]`).
- The macOS/Linux guest path is unchanged: it still uses the `VZVirtioSocketDevice` directly.
  `asb_core_mac.m` picks per `os_type` — Windows guest → ivshmem host transport; otherwise VZ.
- The existing `vm_ssh_proxy_ivshmem.m` already proves the TCP↔ch7 ring bridge; fold its ring code
  into the shared host transport so there's one ivshmem reader, not several.

No interface decision is needed here — it's a contained host module (task #21) consumed by the
existing helpers.

## 5. Display / input / audio / clipboard host components (task #21)

The test viewer (`asb_viewer.m`) proved the host logic; production needs it inside the app, owned by
the launcher, not a separate process:

- **ivshmem directory ownership** (`[[ivshmem-directory-ownership]]`, task #33): the **VM launcher**
  (the `QemuVm` start path) publishes the `AsbShmDirectory` into the `memory-backend-file` *before
  the guest boots*; the display window + transport helpers become pure consumers (read the existing
  directory, discover regions by channel id, never memset/publish). This is what makes "the VM
  succeeds whether or not the display window is open."
- **Display window** `AsbDisplayWindow` (Windows-guest peer of `VzDisplayWindow`): an NSView that
  renders ch2 VDD wire frames + cursor (port the `display_recv_thread`/`drawRect`/NSCursor logic
  from `asb_viewer.m`), and captures NSEvents → ch3 input packets (coalesced mouse per tick). Wired
  into `handle_vm_state_change`/`asb_mac_open_display` exactly where `VzDisplayWindow` is today. It
  reuses the existing `asb_mac_vm_set_audio_muted`/`set_clipboard_sync` agent hooks on
  show/close/focus (as `vz_display.m` does), **and** must send **`idd_connect`** over ch1 on open —
  the Windows host display engine does this (`vm_display_idd.c:1536`) so the guest agent respawns the
  input/clipboard helpers into the active console session. Without it, input/clipboard can target the
  wrong session.
- **Audio** (ch4 → CoreAudio AudioQueue) and **clipboard** (`VmClipboardMac` over ch5/6, on the
  unified transport). 9P (ch9P) optional for parity (host→guest file share).

## 6. The `QemuVm` backend (Windows guest VM lifecycle)

New `src/backend_mac/qemu_vm.{h,m}`, the `VzVm` peer used when `os_type=="Windows"`:
- Launches the **vendored patched QEMU** (`-accel hvf`, EDK2 AAVMF firmware, `-display none`,
  NVMe `disk.img`, `vmnet`/NAT net, USB kbd/mouse/tablet, `ivshmem-plain` +
  `memory-backend-file=<bundle>/ivshmem.bin`, guest Secure Boot off when testMode). Recipe is the
  validated dev boot script (`/tmp/boot_ivshmem.command`) turned into a generated, per-VM argv.
- Exposes the same surface `asb_core_mac.m` already calls on `VzVm`: a state callback
  (Starting/Running/Stopping/Stopped — derived from the QEMU process + QMP), `start`, `requestStop`
  (graceful = agent `shutdown`, fallback QMP `system_powerdown`, **never** kill, `[[shut-down-vms-safely]]`),
  `stop` (force). Map QEMU/QMP events onto the `VZVirtualMachineState` values
  `handle_vm_state_change` already switches on, so the core state machine is reused unchanged.
- Owns the ivshmem backing file (create/size before launch) and publishes the directory (§5).
- VM bundle gains `ivshmem.bin` (+ QEMU NVRAM vars copy); `disk.img` is the raw NTFS/GPT image the
  engine wrote. `vm_dir.m` gets Windows-guest accessors; no `aux/hardware/machine-id` for Windows.

**Validated mapping** — the HCS Windows compute system (`build_vm_config`/`hcs_create_vm`,
`hcs_vm.c:900-1204`) → QEMU+HVF:
- `ComputeTopology.Memory.SizeInMB` / `Processor.Count` → `-m` / `-smp`.
- `Scsi.Primary.Attachments.0 = VirtualDisk` (`:1167`) → NVMe `disk.img` (raw; inbox NVMe driver,
  validated). No install ISO / resources ISO attached at boot — HCS only adds those at SCSI 1/2
  when present (`:919-933`); our offline build attaches neither.
- `Chipset.Uefi` + SecureBoot template (`:943-959`) → EDK2 AAVMF. **v0 = testMode**: Secure Boot
  **off** + `bcdedit testsigning on`, because our VDD/ivshmem/VAD are test-signed (matches the dev
  VM). Production-signed drivers + Secure Boot is a later item.
- `HvSocket.HvSocketConfig.ServiceTable` (ports 1–6 GUIDs, `:1105-1129`) → **replaced entirely** by
  `-device ivshmem-plain` + `-object memory-backend-file`; the guest auto-detects it (§2).
- `VideoMonitor` is **required** (`:1019`, vmwp crashes without it) → QEMU keeps a display device
  even under `-display none`; the VDD is the real display over ch2.
- `GuestState` VMGS/VMRS (`:1012-1017`) → QEMU UEFI NVRAM vars file (per-VM pflash vars).
- `SecuritySettings`/vTPM is **x64-only** (`:1048-1057`, ARM has none) → **no swtpm needed** for the
  ARM64 Windows guest (corrects §8). `Keyboard`/`Mouse` → USB kbd + tablet. `NetworkAdapters`
  (HCN endpoint) → `vmnet` NAT. `ComPorts` is Linux-only; Windows omits it (optional host-side
  debug serial).

## 7. The disk builder + provisioning bake (workstream C)

### Windows has three disk-build methods — we mirror the primary one
1. **VHDX-first / offline-inject (PRIMARY, `--to-vhdx`).** `vhdx_create_thread` (`asb_core.c:1039`)
   generates `unattend.xml` (`generate_unattend_vhdx`, specialize+oobeSystem only),
   `setup.cmd` (`generate_vhdx_setup_cmd` — just `agent --install`, binaries already on disk),
   `SetupComplete.cmd` (`generate_vhdx_setupcomplete` — drivers from `%SystemRoot%\AppSandbox\
   drivers`), and a **manifest** (`generate_vhdx_manifest`, `disk_util.c:1764`); then runs
   `iso-patch.exe --to-vhdx <iso> 1 <gb> --output <vhdx> --stage <manifest>` which inits GPT,
   `dism /Apply-Image`, `bcdboot`, and stages the manifest **offline** (`iso-patch.c do_to_vhdx`).
   Everything is inserted up front **except GPU drivers**. ← **This is what the Mac mirrors.**
2. **Resources-ISO + in-guest Windows Setup** (`iso_create_resources`, `disk_util.c:642`):
   blank VHDX + a resources ISO carrying `autounattend.xml` (with a windowsPE pass that partitions
   and installs the WIM) + agent/drivers/`SetupComplete.cmd`; HCS boots the install ISO and Setup
   runs autounattend. The OS install happens **in-guest**. (Not used on Mac — we author offline.)
3. **Template / differencing** (`vhdx_create_differencing` + `iso_create_instance_resources` +
   `generate_unattend_vhdx_template` sysprep, `asb_core.c:2921`): clone a sysprepped template VHDX
   as a child disk with a minimal per-instance unattend. (A later parity item, not P-series.)

GPU drivers are never baked in any method — on Windows they're shared into the guest at runtime via
9P for GPU-PV (`generate_vhdx_manifest` skips them; `hcs_vm.c:1059`). On Mac there is no GPU, so the
question is moot.

### The Mac `build-windows` (mirror of method 1, applied by our engine)
**Engine gap (verified):** `engine/win_disk.c` today is `win_disk <install.wim> <out.img>
[image_idx] [disk_gib]` (`win_disk.c:342-344`) — it applies the WIM and injects **one hardcoded**
`UNATTEND_XML[]` string at `\Windows\Panther\unattend.xml` (`:155-214`, `:396-410`). It does **not**
take a manifest, stage arbitrary files, or generate a per-VM unattend. So this is real engine work
(the primitive `ntfs_add_file` exists, `:296`/`:405`, so it's tractable): add (a) a generated
per-VM unattend input (name/user/password/testsigning/locale, replacing the static string) and
(b) a manifest-staging loop (`<host-src>\t<guest-dest>` lines → `ntfs_add_file` into the image).

Add a `build-windows` subcommand to `tools/iso-patch-mac/iso-patch-mac.m` and an app-side
`IsoPatchMac buildWindowsDiskWithName:…`. Unlike the macOS `stage` (which mounts the VM's APFS
volume and needs root), `build-windows` only **reads** the input ISO (`hdiutil attach` read-only)
and **writes a fresh `disk.img` the app already owns** — so it runs **unprivileged**, like
`installMacOSWithName:` (`runUnprivilegedArgs`, `iso_patch_mac.m:235`), and emits the same
`STATUS/PROGRESS/LANG/DONE/ERROR` protocol. The CLI generates the Windows file layout internally
(it knows the fixed guest dests) from a payload dir + VM params, rather than the app building a
2-column manifest. Steps:

1. **Mount the user's Windows ISO** (`hdiutil attach`), read `sources/install.wim` (index 1) and
   detect the default UI language (analogue of Windows' `dism /Get-WimInfo` → the `LANG:` regen).
2. **Build the bootable disk** with `engine/win_disk`: GPT (ESP + MSR + NTFS C:), apply the WIM
   (our reader), patch BCD (our patcher), bootmgfw at `\EFI\Microsoft\Boot\bootmgfw.efi`. (No
   dism/bcdboot — Windows-only.) Output a raw `disk.img` (QEMU NVMe), not a VHDX.
3. **Stage the payload via the same manifest contract** (guest-relative destinations, from
   `generate_vhdx_manifest`, `disk_util.c:1781-1908`):
   - `unattend.xml` → `\Windows\Panther\unattend.xml`
   - `setup.cmd` → `\Windows\AppSandbox\setup.cmd`; `SetupComplete.cmd` → `\Windows\Setup\Scripts\`
   - `appsandbox-agent.exe` + `-input/-displays/-clipboard/-clipboard-reader/-audio.exe` →
     `\Windows\AppSandbox\` (the **ivshmem-wired** builds)
   - `AppSandboxVDD.{dll,inf,cat,cer}` + `devcon.exe`, `AppSandboxVAD.{sys,inf,cat,cer}`, **and the
     new `asb_ivshmem.{sys,inf,cat,cer}`** → `\Windows\AppSandbox\drivers\`
   - OpenSSH MSI → `\Windows\AppSandbox\` when sshEnabled; **GPU files omitted** (no GPU on Mac)
4. **First-boot scripts** (the `generate_vhdx_*` content): `setup.cmd` runs `agent --install`;
   `SetupComplete.cmd` does `testsigning on`, `certutil -addstore Root/TrustedPublisher`,
   `devcon install` for **ivshmem first**, then VDD, then VAD, `powercfg` disable display sleep,
   and (sshEnabled) OpenSSH `msiexec` + `sc config sshd auto` + `net start sshd`; log to
   `C:\Windows\AppSandbox\setup.log`. unattend (specialize+oobeSystem) is `generate_unattend_vhdx`.
5. **SSH key:** sshd is provisioned by the bake; the **authorized key is deployed at runtime by the
   agent** over ch1 (`ssh_deploy_key`), exactly as today on both hosts — host passes the AppSandbox
   pubkey via `agent.deployKeyLine` (`asb_core_mac.m:514`). No private key ever touches the guest.

**Guest payload source:** these binaries must be **locally built + signed with the stable
AppSandbox cert** (same cert as VDD/VAD) and shipped in the Mac app bundle under
`Contents/Resources/agent_win/` (peer of `agent_mac/`, discovered like
`agent_resource_directory()`, `asb_core_mac.m:320`). Building them requires the Windows toolchain
(VS2022 + EWDK) — produced on the dev VM and vendored into the bundle as a build input.

## 8. Vendoring (host runtime, QEMU+EDK2 only)

Into the app bundle / runtime (peer of how `iceman/Vendor/qemu` is laid out today):
- Patched **QEMU** `qemu-system-aarch64` (+ the 3-line ivshmem patch) and required share/firmware
  (**EDK2** AAVMF `edk2-aarch64-code.fd` + writable vars template). MIT/GPL notices updated in
  `THIRD-PARTY-NOTICES.md`. Code-sign + entitlements for HVF (`com.apple.security.hypervisor`).
- **No vTPM / swtpm needed** — verified: ARM Windows HCS exposes no vTPM (`SecuritySettings` is
  x64-only, `hcs_vm.c:1048-1057`) and the Win11 checks are bypassed via the unattend LabConfig keys.
  v0 runs Secure Boot off + testsigning, so no TPM device is required.
- The signed Windows guest payload (§7) under `Resources/agent_win/`.
- Privileged helper / AEWP path already exists for `iso-patch-mac`; `build-windows` reuses it.

## 9. UI / API surface (workstream F)

- `web/app.js`: on a Mac host, allow `osType == "Windows"` (and `Linux` later); show the Windows
  config fields (already present — they're the Windows-host fields). **GPU control shown but
  forced/greyed to None on Mac** (tooltip: "GPU-PV is not available on macOS"). Require an
  `install.wim`/ISO picker for Windows (peer of the `.ipsw` picker). Keep the per-OS name/user/
  password validation that already exists.
- `src/app_mac/ui.m`: `handleCreate` already passes `osType`/`gpuMode` through — just route Windows
  to the new core branch (no lock to macOS).
- `src/app_mac/headless.m`: `validate_create_mac` (`:318`) currently hard-rejects non-macOS
  (`:322` "Only macOS guests are supported"). Open it for `osType=="Windows"`: apply the Windows
  name rules (≤15 NetBIOS, no all-digits, no leading/trailing hyphen — app.js already encodes them),
  keep the numeric rules, force `gpu_mode=GPU_NONE`. Keep `templates`→501 and `snapshots`→501 for
  v0 (`capabilities:{snapshots:false,templates:false}`, `:623`). Status/edit/delete/shutdown-daemon
  guards already key off `vm_installing` + `running` (`:773`, `:863`, `:964`), so they work as-is.
- **Installing state during the engine build (verified requirement):** the harness asserts
  edit/delete-during-build → 409, which headless derives from `vm_installing(v)` =
  `!install_complete && install_progress >= 0` (`headless.m:283`). So the Windows-on-Mac create must
  keep the VM in that state for the whole `build-windows` phase (set `install_progress >= 0`,
  `install_complete=false` until the agent connects) — exactly as the macOS install path does via
  `update_install_progress`/`finish_install` (`asb_core_mac.m:603,662`). `building` stays `@NO`;
  `derive_state`→"installing" is what the harness keys on.
- Persist Windows VMs in `vms.cfg` via the existing save/load (fields already present:
  `asb_core_mac.m:163-271`).

## 10. Phasing (each phase shippable, PC untouched, no shims)

> Status: **P0–P5 DONE** (implementing files in the Status section up top). **P6** — UI/API (§9) done
> in `headless.m`/`app.js`; vendoring (§8) partial. **P7** — acceptance (`run_all.py` Windows spec
> green on a Mac host) remains. P6 vendoring + P7 are the path to v0.

- **P0 — finish the harness correctness:** task #33 (launcher publishes the ivshmem directory;
  viewer/consumers read-only). Unblocks "VM up without the window" and the current black screen.
- **P1 — host-side ivshmem transport (§4, task #21):** give the existing helpers (`VmAgentMac` /
  `VmSshProxyMac` / `VmClipboardMac`) an ivshmem-backed fd per channel for a Windows guest; VZ path
  for macOS/Linux guests unchanged. Includes parsing `idd_status:` in `VmAgentMac` (§11).
- **P2 — `QemuVm` backend (§6)** + ivshmem directory ownership wired into launch; boot the existing
  dev disk under the app's launcher, agent online over ivshmem via the new transport.
- **P3 — production display/input/audio/clipboard host components (§5, task #21):** port from
  `asb_viewer.m` into `src/backend_mac`; `AsbDisplayWindow` wired into the core state machine.
- **P4 — disk builder `build-windows` + provisioning bake (§7)** incl. SSH/sshd (task #29);
  app-side `IsoPatchMac buildWindowsDiskWithName:`; build+sign+vendor the Windows guest payload.
- **P5 — core `os_type=="Windows"` create branch (§3)** in `asb_mac_vm_create`: WIM picker →
  build-windows → autostart → QemuVm; `gpu_mode` forced none.
- **P6 — UI/API (§9)** + **vendoring (§8)**.
- **P7 — acceptance:** the headless `run_all.py` Windows spec (`testMode, sshEnabled, sshDeployKey,
  user/test123`) green on a Mac host: build → boot-to-online → `display_ready` (VDD `idd_status:ok`)
  → SSH key deployed + key-only login → graceful shutdown → force-stop → delete; snapshots/templates
  return their documented codes.

## 11. Risks & open items

- **Display readiness signal (DONE).** The guest agent sends `idd_status:ok` over ch1 after `hello`;
  the Mac `VmAgentMac` now parses it (`vm_agent_mac.m:325`) and surfaces `iddReady` into the
  Windows-guest `display_ready` gate (`running && agentOnline && iddReady`), mirroring the Windows
  `asb_vm_idd_ready`. macOS guests keep `running && agent_online`. No guest change (emitted over ch1).
- **`idd_status` depends on devcon (verified on the dev VM, P0 reboot).** The guest agent's
  `report_idd_status` shells out to `devcon` to query the VDD driver state; if devcon isn't at the
  path it invokes, it logs `devcon failed to launch (2)` and reports `idd_status:not_found` **even
  when the VDD is running** (confirmed: live interactive desktop while the agent reported not_found).
  Since `display_ready = running && agent_online && idd_ready` and `idd_ready` latches from
  `idd_status:ok`, the P4 disk bake MUST stage devcon where the agent invokes it (the manifest already
  stages devcon to `\Windows\AppSandbox\drivers\` — ensure the agent uses that path), or P7's
  "display ready when online" check fails. (Alternatively, make the agent's VDD-state query not
  depend on devcon.)
- **`idd_connect` on display open (verified).** The Windows host display engine sends `idd_connect`
  over ch1 when opening the display (`vm_display_idd.c:1536`); the guest agent then respawns the
  input/clipboard helpers into the active console session (`agent.c handle_idd_connect:1698`). The
  Mac `AsbDisplayWindow` open path must send the same, or input/clipboard can bind the wrong session.
- **ch1 post-hello is NOT a blocking handshake (verified).** The guest `handle_client` (`agent.c:1788`)
  loops on `asb_poll` and treats `gpu_none`/`set_ip:` as optional commands — it never waits for them.
  So the Mac side needs only: parse `idd_status:`, send `ssh_enable` (already done), send
  `idd_connect` on display open. `gpu_none` is unnecessary (gpu always none); `set_ip:` is
  unnecessary (QEMU vmnet/DHCP gives the guest its IP, vs HCS NAT which needs the agent to set it).
- **QEMU lifecycle → VZ state mapping.** `handle_vm_state_change` is written against
  `VZVirtualMachineState`. `QemuVm` must synthesize those transitions faithfully (process exit,
  QMP `SHUTDOWN`, graceful vs forced) so the reused state machine behaves.
- **Agent shutdown hang** (`[[agent-shutdown-hang]]`): guest helpers don't honor
  WM_QUERYENDSESSION/SERVICE_CONTROL_SHUTDOWN, so graceful stop can stall ACPI. Fix before P7 (the
  `/f` workaround is not acceptable for the product path).
- **Signing pipeline:** stable AppSandbox cert for `asb_ivshmem.{sys,cat}` (same as VDD/VAD), baked
  + installed by the first-boot script. The whole guest payload must be reproducibly built/signed.
- **vmnet privileges** for NAT networking under QEMU (entitlement / privileged helper).
- **WIM input UX:** user supplies `install.wim`/ISO (we extract `sources/install.wim`); decide image
  picker + validation (edition index, arch == arm64).

## 12. Cross-references
`windows-on-mac-plan.md` (R&D substrate, workstreams A–G), `asb-transport-design.md`,
`interactive-vm-runbook.md`, `ivshmem-transport-runbook.md`, `qemu-ivshmem-build.md`. Memory:
`[[ivshmem-directory-ownership]]`, `[[ivshmem-reconnect-semantics]]`, `[[ssh-over-ivshmem]]`,
`[[iso-patch-clean-room]]`, `[[appsandbox-architecture]]`, `[[windows-on-mac-vmm-decision]]`,
`[[no-external-deps-constraint]]`, `[[shut-down-vms-safely]]`, `[[run-vms-and-builds-via-gui]]`.
