# Interactive Windows VM on macOS — Runbook & Implementation Notes

> Living record of the interactive Windows-guest-on-Mac path built on the ivshmem transport.
> Status (2026-06-16): **display + input + cursor + audio working**, headless QEMU, from the real
> product binaries (modified in place, NOT cloned). Clipboard is DONE (§8). **This is the dev-harness / bring-up record** (test viewer `asb_viewer.m`,
> `/tmp/ivshmem.bin` published by the viewer, dev VM `192.168.1.28`). The **PRODUCTION host is now the
> headless daemon** — `AppSandbox --headless` + `tools/headless-api/asb.py` driving `src/backend_mac`
> (`qemu_vm.m`, `idd_display.m`); the launcher (not the viewer) owns the ivshmem directory. Live VM =
> `MyAppSandbox` at `192.168.2.2`, ssh-localhost over ivshmem ch7 at `127.0.0.1:<ssh_info port>`. The
> manual viewer/boot steps below are the bring-up harness, not the shipping path. Companion docs:
> `windows-on-mac-plan.md` (overall plan), `ivshmem-transport-runbook.md` (transport reproduction),
> `asb-transport-design.md` (channel layer).

## 1. What works
| Feature | Guest side | Host side (`asb_viewer`) | Channel |
|---|---|---|---|
| Display | `tools/vdd/vdd.cpp` frame server → `asb_transport` (`asb_listen(ASB_CH_DISPLAY)`/`asb_accept`/`asb_send`) — the SAME emit path it uses over HvSocket on Windows | `display_recv_thread` reconstructs the VDD wire stream (full + dirty-rect frames) into a framebuffer, renders @60fps | ch2 STREAM |
| Cursor  | `vdd.cpp` `VddSendCursorUpdate` → `VDD_WIRE_CURSOR_HEADER` (+ shape) over the **same ch2 stream** | parses cursor msgs, sets `NSCursor` (ALPHA + MASKED_COLOR/XOR, content-scaled for retina) | ch2 STREAM |
| Input   | `tools/agent/appsandbox-input.c` → `asb_transport` (`asb_listen(ASB_CH_INPUT)`) | NSEvent → `InputPacket` → slot **h2g** ring (coalesced moves) | ch3 STREAM |
| Audio   | `tools/agent/appsandbox-audio.c` → `asb_transport` (`asb_listen(ASB_CH_AUDIO)`) | drains slot **g2h** → CoreAudio AudioQueue | ch4 STREAM |
| Clipboard | `appsandbox-clipboard.c` (ch5) + `appsandbox-clipboard-reader.c` (ch6) | full engine: text/RTF/HTML/image/files, both directions | ch5/ch6 STREAM |
| Agent control | `tools/agent/agent.c` → `asb_transport` (`asb_listen(ASB_CH_AGENT)`) | `agent_control_thread` connector: hello/heartbeat/idd/ping↔ok | ch1 STREAM |
| SSH proxy | `agent.c` `ssh_proxy_thread` → `asb_listen(ASB_CH_SSH)`; relay branches PC `select()` / ivshmem `asb_poll` | (publish a ch7 slot to use) | ch7 STREAM |

All channels ride one 128 MiB ivshmem BAR. **Every channel — display included — now uses one
`asb_transport` code path**: on PC `asb_*` are the exact `AF_HYPERV` calls each component already made
(byte-identical to Windows→Windows/Linux); on Mac they bind ivshmem stream slots. The VDD is the real
product driver; input/audio/clipboard are the real product EXEs spawned by the real
`appsandbox-agent.exe` service. See §12 for why the display uses the VDD's existing accept-driven
frame emit instead of a bespoke shared-memory region.

## 2. Shared-memory layout (`/tmp/ivshmem.bin`, published by the viewer)
- `0x00000` — `AsbShmDirectory` (1 page): magic `ASBSHMD1`, region table.
- `0x10000` — **ch2 display slot** (≤16 MiB used of a 48 MiB region): one `AsbSlot` with a 16 MiB
  **g2h** ring (the VDD pushes the frame/cursor wire stream) + a tiny unused h2g ring.
- `0x3010000` — **ch3 input slot** (64 KiB): one `AsbSlot` (state + g2h + h2g rings, 4 KiB each).
- `0x3020000` — **ch4 audio slot** (1 MiB): one `AsbSlot` (256 KiB rings).
- `0x3120000`/`0x3520000` — ch5/ch6 clipboard slots (1 MiB rings); `0x3920000` — ch1 agent slot.
Stream slots: host is the **connector** (arms `CONNECTING`), the guest (VDD or agent-spawned EXE) is
the **acceptor** (`asb_accept`). Display + audio flow guest→host (**g2h**); input flows host→guest (h2g).

## 3. The Mac host (test viewer) — `tools/transport/test/asb_viewer.m`
Independent test host (the production Mac host will live in `src/backend_mac`, Workstream D). Cocoa +
AudioToolbox. mmaps the ivshmem file, **publishes the directory** (frame + input + audio regions),
renders ch2 @60fps, mirrors the guest HW cursor to `NSCursor`, captures NSEvents → input ring
(mouse moves coalesced to ≤1/tick + flushed before discrete events), and plays ch4 audio via a
CoreAudio AudioQueue fed from a 1 MiB PCM jitter buffer.
Build: `clang -O2 -fobjc-arc -o asb_viewer asb_viewer.m -framework Cocoa -framework AudioToolbox`

## 4. Build toolchain (dev VM 192.168.1.28, `[[vm-build-toolchain]]`)
- **User-mode binaries** (agent/input/audio/clipboard): **VS2022** at `D:\VS2022` (registered; find via
  `vswhere` → `VsDevCmd.bat -arch=arm64 -host_arch=arm64`, then vanilla `cl`). NO INCLUDE/LIB tweaks.
  Repo script: `tools/agent/build-arm64.cmd`.
- **VDD only**: **EWDK** (ISO at `D:\ewdk.iso`, mount → `E:`; `SetupBuildEnv.cmd x86_arm64` → msbuild).
  Repo build script lives in `C:\asb\build_vdd.cmd` on the VM.
- NEVER redirect `VsDevCmd`/`SetupBuildEnv` to `>nul` (breaks env). Edit canonical sources on the Mac,
  `scp` to `C:\asb\appsandbox\...`, build there.

## 5. Deploy (real-provisioning layout) — `tools/agent/deploy-arm64.cmd`
Binaries → `C:\Windows\AppSandbox\`; `appsandbox-agent.exe --install` → **`AppSandboxAgent`** service
(LocalSystem). Its monitor threads spawn input/clipboard/audio into the **console session** via
`CreateProcessAsUserW` — independent of the (still AF_HYPERV) control channel, so channels work even
though the agent control channel (task #18) isn't wired yet. The VDD is installed as a root device
`Root\AppSandboxVDD` (devcon) — proper redeploy: `devcon remove` → `pnputil /delete-driver oemNN.inf
/uninstall /force` → `devcon install` (same `DriverVer` else stale DLL persists; never hand-copy into
`System32\drivers\UMDF`).

## 6. Bring-up after a VM boot
1. Boot: `open -a Terminal /tmp/boot_ivshmem.command` (`-smp 4 -m 4096`, **`-display none`** — the QEMU
   window caused input lag; `[[ivshmem-viewer-display-none]]`). Enter sudo password.
2. Re-mount EWDK if you'll build the VDD: `Mount-DiskImage D:\ewdk.iso`.
3. Launch viewer **as the logged-in GUI user, NOT root**: `launchctl asuser $(id -u <user>) sudo -u <user>
   .../asb_viewer /tmp/ivshmem.bin`. The shell here is root; if the viewer runs as root, received
   clipboard files land in root's private temp (`/var/folders/zz/.../T`, mode 700) which the user's
   Finder can't read → file paste fails. (The pasteboard itself is shared via the macOS pasteboard
   server, so text/image *data* works either way; only filesystem-backed formats need the right user.)
   ivshmem.bin is mode 666 so the user can mmap it.
4. **Relaunching the viewer alone fully reconnects every channel — no helper bounce, no `devcon`
   needed.** Each guest server (VDD ch2, input, audio, clipboard, agent) detects the host re-arming
   its slot and re-accepts (see §7 "reconnect"). The relaunched viewer arms each slot `CONNECTING`;
   the VDD `asb_accept`s ch2 and sends a fresh full frame; the helpers re-accept their slots.
5. The agent auto-starts and respawns input/audio/clipboard into the console session. They connect
   once the viewer has published and **reconnect automatically on every subsequent viewer relaunch**.

## 7. Gotchas (all the ways this bit us)
- QEMU `-display cocoa` → input lag. Use `-display none`.
- EWDK `E:` unmounts on VM reboot → VDD build = "msbuild not found". Re-`Mount-DiskImage`.
- Same `DriverVer` → `devcon install` keeps the stale store package. Purge + reinstall.
- Mouse-move flood (retina trackpad) overruns the 204-slot input ring → drops/lag. Coalesce on host.
- **Channel reconnect across viewer relaunches** (FIXED — was: helpers stuck `CONNECTING`, input/audio
  dead until bounced): the guest servers (input/audio/clipboard/agent — all real product EXEs) use the
  same `accept → serve → re-accept` loop as on Windows. The fix is in `asb_transport`: an ivshmem
  connection that leaves `ESTABLISHED` (the connector/viewer re-armed the slot to `CONNECTING` on
  relaunch, or closed it) now reports disconnect — `asb_recv`→0, `asb_poll`→<0, `asb_send`→fail —
  exactly like a TCP peer close drives the re-accept loop on PC. And `asb_close` only marks the slot
  `CLOSING` if it's still `ESTABLISHED`, so a helper tearing down its stale connection never clobbers
  the connector's fresh `CONNECTING`. Audio is a *sender*: when the guest is silent it isn't calling
  `asb_send`, so `stream_loop` adds an idle `asb_poll(s,0)<0` liveness check to notice the re-arm.
  Net: relaunch the viewer as many times as you like — input/audio/clipboard/agent all re-establish on
  their own (verified: ch3/ch4/ch5/ch6/ch1 all return to `ESTABLISHED` with no bounce).
- **Black display after a viewer relaunch** (FIXED, see §12): the display channel is now an
  accept-driven stream — a relaunched viewer arms the ch2 slot `CONNECTING`, the VDD `asb_accept`s and
  sends a fresh full frame, exactly like reopening the IDD window on Windows. (Earlier the display was
  a passive shared-memory region whose header the viewer could clobber; that whole region is gone.)
- **ch2 single-slot reconnect race** (FIXED): a relaunched viewer re-arms the *same* ch2 slot. The
  VDD must NOT `asb_close` the old connection when adopting the re-armed one (that marks the shared
  slot `CLOSING` and kills the new connection → `resend failed` loop). Instead it `asb_abandon`s the
  stale wrapper (frees it, leaves the slot to the new owner) and `asb_stream_reset`s so the fresh full
  frame aligns at ring offset 0. On PC each connect is a distinct socket, so the old one is closed
  normally.
- IddCx acquired surface only has DIRTY regions current → VDD accumulator publishes full coherent frames.
- VM had no audio endpoint until the virtual-audio driver was installed.

## 8. Clipboard (ALL formats) — guest side DONE, host engine pending
Mirror `src/backend_win/vm_clipboard.c` (Windows↔Windows; NOT `vm_clipboard_mac.m` = macOS guests):
ch5 writer (Mac→Win: host sends FORMAT_LIST, serves DATA_REQ) + ch6 reader (Win→Mac: guest sends
FORMAT_LIST, host fetches via DATA_REQ/DATA_RESP).
- **Guest side DONE:** `appsandbox-clipboard.c` (ch5) + `appsandbox-clipboard-reader.c` (ch6) wired to
  `asb_transport` (same swap as input/audio; protocol bytes untouched). Builds clean.
- **`asb_transport` PC-parity fixes (to keep Windows identical):** `asb_send` now loops until all bytes
  sent on the PC path (a single `send()` truncates large clipboard/file buffers); `asb_listen` retries
  `bind()` 10×/500ms. Both match the originals' `send_all`/bind-retry.
- **Host engine DONE (in `asb_viewer`):** publishes ch5 + ch6 stream slots (1 MiB rings, 4 MiB regions);
  two connector threads — `clip_writer_thread` (ch5: poll NSPasteboard changeCount → FORMAT_LIST + serve
  DATA_REQ) and `clip_reader_thread` (ch6: recv FORMAT_LIST → DATA_REQ each handled fmt → DATA_RESP →
  set NSPasteboard). **All formats:** CF_UNICODETEXT↔string (UTF-16↔UTF-8), "Rich Text Format"↔RTF,
  "HTML Format"↔HTML (CF_HTML wrapper parse/build), CF_DIB↔image (BMP header add/strip ↔ PNG via
  NSBitmapImageRep), CF_HDROP↔file URLs (file-transfer sub-protocol: `ClipFileInfo`+chunks, dir
  recursion, temp dir). Echo suppression via `g_clipSuppress` (changeCount we set). **VALIDATED both
  directions** — text, RTF, HTML, images, files/folders. Two host-side fixes were required: (a)
  NSPasteboard writes must run on the **main thread** (`dispatch_sync(main_queue)`) — `writeObjects`
  for files won't register from a background thread; (b) the viewer must run **as the logged-in GUI
  user, not root** (§6.3) so received files land in a temp dir the user's Finder can read.

## 9. Build must define UNICODE
`build-arm64.cmd` passes `/DUNICODE /D_UNICODE` to match the production `.vcxproj` `CharacterSet=Unicode`.
Without it, `TEXT()`/`SE_*_NAME`/`TCHAR` macros resolve to ANSI (C4133 on `SE_SHUTDOWN_NAME`, and a
different character set than production). Keep it.

## 10. Windows-parity audit of the wired channel EXEs (vs main)
Identical except: (1) `appsandbox-input.c` now retries bind (input originally did not; the other three
did — now uniform); (2) accept-failure retry sleep 100ms vs originals' 1000ms (cosmetic); (3)
`WSAStartup` moved into `asb_transport_init` (still before any socket use). Wire protocols, accept model,
threading unchanged. Compiles clean (UNICODE); not yet re-tested on a Windows host.

## 11. Agent control (ch1) + SSH proxy (ch7) — `agent.c` wired, ch1 VALIDATED over ivshmem
`agent.c` is now wired to `asb_transport` (task #18). **Zero functional change to the PC
(Windows→Windows / Windows→Linux) path is the hard guarantee**, achieved by keeping `asb_*` as thin
AF_HYPERV wrappers (same service GUID `a5b0cafe-000N-…`, `asb_poll`=`select`, `asb_recv/asb_send`=
`recv`/looped-`send`, `recv_line/send_line` byte-identical) and by branching the SSH relay:
- **`ssh_relay_thread`**: if `asb_conn_socket_u64(hv) != INVALID_SOCKET` (PC) → the **original blocking
  dual-fd `select()`** relay, byte-for-byte as before; else (ivshmem) → the poll-driven relay. So the
  Windows SSH proxy behaves exactly as it did; the poll path only runs on Mac.
- Removed the stale `g_listen_sock` close (listener is owned by `listener_thread` via
  `asb_close_listener`; its accept loop polls `g_stop_event` every 1s). `g_client_sock` is now `AsbConn*`
  (`!= NULL`, not `!= INVALID_SOCKET`).
- **Build**: `build-arm64.cmd` now compiles `..\transport\asb_transport.c` + `ws2_32.lib` into
  `appsandbox-agent.exe`. All five binaries build clean (UNICODE).
- **ch1 VALIDATED over ivshmem (2026-06-17):** added a ch1 AGENT stream region (128 KiB @ `0x3920000`,
  64 KiB rings) + `agent_control_thread` connector to `asb_viewer.m`. Agent log: `Listening … (transport=
  ivshmem)` → `Client connected.` → `Command: 1:ping`. Viewer log: `hello` → `idd_status:not_found` →
  `1:ok` (tag preserved) → `heartbeat`. Full bidirectional request/reply + heartbeats confirmed.
- **ch7** uses the identical `asb_listen`/`asb_accept`+relay structure (gated by `ssh_enable`); wiring
  proven by ch1, not yet separately exercised (would need the viewer to publish a ch7 slot + drive
  `ssh_enable`).

## 12. Display = the VDD's own accept-driven frame emit (one code path, PC + Mac)
The Windows host display client is `src/backend_win/vm_display_idd.c`. When the IDD window opens (or
reopens), `idd_recv_thread_proc` connects a **fresh** ch2 socket and reads `magic`+header+pixels — it
sends **no bytes**, no handshake. The VDD's listener `accept()`s and, purely as a result, resets
`frameSeq=0; bSentFullFrame=FALSE` → the next frame is full. Reconnect = new accept = fresh full frame.

The Mac path now uses that **exact same emit method**, routed through `asb_transport` (the move we made
for the agent + channel EXEs): the VDD frame server calls `asb_listen(ASB_CH_DISPLAY)` / `asb_accept` /
`asb_send` instead of raw winsock. On PC those compile to the identical `AF_HYPERV` calls (Windows path
byte-identical); on Mac they bind the ivshmem ch2 stream slot. The bespoke shared-memory frame region
(`asb_frame_open`/`VddWriteIvshmemFrame`/`AsbFrameRegion`/cursor area) is **deleted** — no `host_epoch`,
no reattach signalling, no passive header. The viewer's `display_recv_thread` is a faithful port of the
`vm_display_idd.c` recv loop: arm `CONNECTING`, read `VDD_FRAME_MAGIC` (full + dirty-rect) and
`VDD_CURSOR_MAGIC` messages, reconstruct into a framebuffer + `NSCursor`, re-arm on slot close.

Why route through the ring instead of zero-copy: a full 1080p frame is ~8 MB ≈ 0.3 ms on the measured
28 GB/s ivshmem, and steady state is small dirty-rect frames — negligible, and it buys one tested emit
path for both hosts. The single-slot reconnect race fix is in §7 (`asb_abandon` + `asb_stream_reset`).
