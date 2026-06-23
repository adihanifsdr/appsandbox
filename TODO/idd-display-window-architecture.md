# IDD Display Window — architecture, divergences, and the robustness fix

**Scope:** the macOS host-side display/input/audio/clipboard window for ivshmem Windows guests,
`src/backend_mac/idd_display.m`, which is the **port of the proven Windows host consumer**
`src/backend_win/vm_display_idd.c`. Branch map: `main` = the shipping Windows-to-Windows product
(`backend_win`); `win-on-mac` = the Windows-on-Mac port (`backend_mac`).

> Lesson that produced this doc: the file header points at the proven peer (`vm_display_idd.c`) for a
> reason. Repeatedly editing the Mac port without first reading the Windows original led to a string of
> wrong diagnoses (casal / ACPI / "the agent prevents shutdown" / daemon-lifecycle). **Read the Windows
> original of any subsystem before reasoning about its Mac port.**

---

## 1. The proven Windows design (`vm_display_idd.c`, ~2494 lines)

Consistent principles, none of which can stall the UI:

- **`send_input` (~L488)** — **non-blocking** single `send()` (socket set `FIONBIO`). `WSAEWOULDBLOCK`
  (buffer full) → **drop the packet, keep the socket** ("rather than stall the UI"). Other error →
  `input_socket = INVALID_SOCKET` (the **recv thread owns the socket and reconnects**). Partial send →
  also flag for reconnect (the guest reads fixed-size 20-byte `InputPacket`s; a truncated one misaligns
  the wire forever). **No lock held across the send.**
- Frame pixels are read into a **thread-local `recv_buf` *before* taking `frame_cs`** (~L1707); the lock
  only ever covers an in-memory copy. **No lock is ever held across a blocking send/recv.**
- The recv thread owns + reconnects display *and* input **together**; render copies are clamped to fixed
  dimensions.
- Threads: `window_thread` (UI/message pump), `recv_thread` (frame+input), `audio_recv_thread`.

## 2. The Mac port (`idd_display.m`, ~1060 lines)

- **Threads:** main (AppKit) + a 60 Hz `NSTimer` `renderTick`, plus 5 reader pthreads — `displayLoop`
  (ch2), `inputLoop` (ch3), `audioLoop` (ch4), `clipWriterLoop` (ch5), `clipReaderLoop` (ch6) — started
  in `-showDisplay`.
- **Locks:** `_fbLock` (framebuffer), `_curLock` (cursor blob), `_pcmLock` (audio jitter), and
  **`_inputLock` = the shared fd-publication lock** — it guards `_inputFd` AND `_displayFd` AND
  `_audioFd` AND `_clipWriterFd` AND `_clipReaderFd`. Teardown takes `_inputLock` to `shutdown()` the
  published fds.
- **Input path:** `renderTick` (main, 60 Hz) → `flushMove` → `-sendInput:` → (originally) a **blocking
  `send`** while holding `_inputLock`.

## 3. Divergences / bugs (Mac port vs the proven original) — all verified in code

| # | Bug | Where | Windows does | Severity |
|---|-----|-------|--------------|----------|
| **M2** | **Blocking `send` on the MAIN thread under the shared `_inputLock`** | `-sendInput:` ~L438; called from `renderTick` ~L392/L461 | non-blocking + drop, no lock | **critical — the freeze** |
| **M1** | `_fbLock` held across the **full-frame `recv`** (multi-MB) → `renderTick` stalls on the lock | `displayLoop` ~L543-546 | recv into `recv_buf` *before* `frame_cs` | high |
| **M4** | `_renderFb` malloc'd once, never resized → **heap overflow** when the guest raises resolution | `renderTick` ~L396-399 | fixed-size texture, clamped copy | high |
| **M3** | `_fbW/_fbH/_fbStride` read unlocked in `copyRenderInfoW`/`drawRect` while `displayLoop` mutates under `_fbLock` → torn geometry vs `_renderFb` size | ~L410/L1206 + L530-534 | reads geometry under `frame_cs` | low |
| **Input reconnect** | Mac splits input onto its own thread (independent reconnect, incl. an EOF poll) | `inputLoop` | reconnects input *with* display in the recv thread | divergence (Mac more robust on idle respawn) |

## 4. The unifying cascade — the whole "wedge" family is ONE bug (M2)

Confirmed by reading + live `sample`. The stuck/spinning window, the `c.shutdown` HTTP timeout / daemon
"wedge", and the orphaned-qemu-on-kill are **not separate bugs** and are **not** the agent/ACPI/shutdown
path:

1. **Guest trigger** (`appsandbox-input.c`): `inject_input` (~L76) does a **per-packet
   `switch_to_input_desktop()`** (OpenInputDesktop→SetThreadDesktop→CloseDesktop) then `SendInput`. The
   ch3 drain stalls when (a) the input helper is mid-respawn (agent/VDD restart → no reader on ch3) or
   (b) a desktop/session transition (lock screen / secure desktop).
2. **Transport** (`asb_transport.c` / `asb_ivshmem_transport.m`): with nobody draining ch3, the bounded
   h2g ring fills → the host pump `asb_ivshmem_pump_main` can't flush it (`ring_write_host` returns 0) →
   it stops draining `internal_fd` → the host socketpair send buffer fills.
3. **Host** (`idd_display.m`): the **blocking `-sendInput:` on the main thread** then blocks → no
   rendering, no input, and the HTTP API (served via the main run loop) stops responding → `c.shutdown`
   times out → force-killing the frozen daemon orphans qemu.

Verified **sound** (not the wedge): the ch1 agent control loop runs on a **serial queue**;
`asb_mac_vm_stop` (`asb_core_mac.m:1405`) dispatches the graceful `shutdown` command on a **background**
queue. Nothing in the agent/shutdown path blocks the main thread.

## 5. The `MSG_DONTWAIT` fix attempt — applied, built, and PROVEN INSUFFICIENT

Applied (mirroring Windows): `-sendInput:` → `send(...,MSG_DONTWAIT)` (full→done, EWOULDBLOCK→drop,
error/partial→reconnect, lock held only for a non-blocking syscall); `displayLoop` full-frame reads into
`scratch` before locking `_fbLock` (M1); `_renderFb` capacity-tracked realloc (M4). `xcodebuild clean
build` = BUILD SUCCEEDED.

**But it does NOT fix the freeze.** Grounded: the rebuilt daemon (binary 12:21, launched 12:23; source
line 461 = `send(fd,&pkt,sizeof(pkt),MSG_DONTWAIT)` confirmed in the running binary) STILL wedges when the
guest "AppSandbox Guest Agent" service is restarted. `sample` shows the main thread pinned **4887/4887 in
`renderTick → sendInput → __sendto` (idd_display.m:461)** despite `MSG_DONTWAIT`. So making the
main-thread send non-blocking is the **wrong altitude** — whether `MSG_DONTWAIT` is unhonored for AF_UNIX
SOCK_STREAM on macOS or the 60 Hz render timer simply spins on it, the main thread stays coupled to ch3's
live state and beachballs on any churn.

## 6. Requirement + the robust fix (NOT YET IMPLEMENTED)

**Requirement:** the in-VM agent, **all** channel helper EXEs, **and** the VDD must each be restartable
without wedging the window.

**Robust fix — the AppKit main thread must do ZERO channel socket I/O:**

- `-sendInput:` ENQUEUES the `InputPacket` into a small bounded ring (drop-**oldest** when full — input
  is droppable) and returns immediately. No socket I/O on the main thread.
- A dedicated worker (fold into the existing `inputLoop`) drains the ring + does the actual `send` +
  owns reconnect-on-drop — exactly like `displayLoop`/`audioLoop`/`clipWriterLoop`/`clipReaderLoop`
  already own their channels on worker threads. A blocking send is fine **off** the UI thread (it
  recovers via EPIPE/reconnect when the channel drops).
- Wake the worker via a condvar (or self-pipe) signalled by `-sendInput:`, for low input latency (a
  poll-only worker would add latency).
- Coalesced-move state (`_hasMove/_moveX/_moveY`) stays on the main thread; only the byte send moves to
  the worker.

After this, every other channel is already on a worker thread, so **any** guest component restart only
churns worker threads and the window is structurally immune to wedging.

## 7. Status of the working tree (`win-on-mac`, uncommitted)

- `idd_display.m`: M1 + M4 applied and correct; the M2 `MSG_DONTWAIT` attempt is applied + built but
  **proven insufficient** — to be superseded by the §6 decouple. The `inputLoop` select/recv EOF
  detection is also applied (idle-respawn reconnect; keep).
- `asb_ivshmem_transport.m` + `asb_transport.c`: casal-free single-writer transport + `asb_recv`
  spin→Sleep(1) backoff. Casal-free part validated; backoff is a CPU fix.
- The VDD on the dev VM was force-reinstalled to the casal-free build (was a pre-casal binary crashing
  with `c000001d`).
- Nothing is committed (per the no-commits-until-v0 rule).
