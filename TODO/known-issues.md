# Known issues — deferred from the `win-on-mac` branch review

Findings from the line-by-line branch review (vs `main`) that were **intentionally deferred**, with the reason. The HIGH + MEDIUM bugs from that review were fixed; these remain.

Last updated: 2026-06-29.

---

## Deferred: shared guest-agent UAFs (touch Windows-to-Windows)

These two are **real** use-after-free races, but they live in the **shared guest agent** (compiled into the Windows guest EXEs for both the AF_HYPERV/Hyper-V and ivshmem transports). The race **pre-exists on `main`** — `main` does `closesocket(client)` outside `g_send_cs` (agent.c main:1810/2048) while senders use `g_client_sock` under the lock; the branch only renamed `closesocket`→`asb_close`. Fixing them would modify the Windows-to-Windows teardown path, so they are deferred to keep the branch's behavioural diff Mac-only. The fix is behaviour-neutral (serialise the free with senders) whenever it's picked up.

| Location | Issue | Fix when taken |
|---|---|---|
| `tools/agent/agent.c:2037-2038` (also 1806-1807) | UAF: `g_client_sock=NULL; asb_close(client)` runs outside `g_send_cs` while sender threads (`ssh_ready`/`ssh_failed`/`os_shutdown`) deref `g_client_sock` under `g_send_cs`. | Take `g_send_cs` around the `NULL`+`asb_close`. |
| `tools/agent/appsandbox-clipboard-reader.c:537,598` | Same UAF: `g_client_sock=NULL` (537) then `asb_close(c)` (598) outside `g_send_cs` while senders use it under the lock. | Close under `g_send_cs` in the client handler. |

Trigger is rare (a host disconnect concurrent with an `ssh_ready`/`ssh_failed`/`shutdown` send). Both paths (Windows + Mac) share the defect.

---

## LOW-priority items — 8 of 9 fixed (2026-06-29)

Fixed: `ntfs.c` USN sentinel clamp (`apply_fixup`), `win_disk.c` leaf-bound before `u16` + `wim_open` NULL-check, `wim_extract.c` NULL-check + `malloc`/`fopen` guards, `bcd_patch.c` malloc guard, `iso_patch_mac.m:463` OpenSSH-MSI download timeout-cancel, and the `asb_ivshmem_transport.m` fd-reuse race (pump closes `internal_fd` under `p->lock`; `-close`'s `shutdown` is guarded by `internal_fd >= 0`).

**Remaining (deferred to the stdin-unification step):**

| Location | Issue | Fix |
|---|---|---|
| `src/backend_mac/iso_patch_mac.m:424` / `:1487` | Admin password passed as a subprocess argv argument (visible in `ps`). This is the only disk-builder path that crosses a process boundary with the secret — Win-on-Win generates the unattend in-process, and Mac-on-Mac uses a 0600 `--user-password-file`. | Move **both** Mac-on-Mac and Win-on-Mac password handoff to **stdin** (`--pass-stdin` sentinel + `NSPipe task.standardInput`), as one isolated change after the other fixes are validated. |

## Deferred: clipboard-receive robustness (pre-existing, surfaced by review verification)

Minor; the path-traversal fix is sound, these are contained (a bad header trips the `CLIP_MAGIC` check at `idd_display.m:1219` and aborts the receive with no escape/partial write).

| Location | Issue | Fix |
|---|---|---|
| `src/backend_mac/idd_display.m:1237` (dir branch) | A hostile `is_directory=1` entry with `file_size>0` desyncs the stream (the directory branch never drains `file_size`). | Drain `file_size` for directory entries too. |
| `src/backend_mac/idd_display.m:1222` | The stray-payload drain ignores the `idd_rd_full()` return. | Harmless (next header read fails → `return NO`); check the return for clarity. |

## Pending verification — needs a Windows agent build

**`tools/agent/agent.c` — idd_status refresh fix is UNTESTED.** The fix (make `report_idd_status` change-gated + `force` flag, and re-call it from the 20s device-check loop so the host's `idd_ready` tracks the live VDD state instead of a one-shot boot snapshot) is implemented but **cannot be built or run on the Mac** — `agent.c` compiles into `appsandbox-agent.exe`, a Windows binary. To verify it:
1. Rebuild `appsandbox-agent.exe` on the Windows dev VM (VS2022).
2. Re-stage it into the disk-builder JIT payload.
3. Create a fresh **concurrent** pair (the repro: 2× 4-core Windows VMs on the 8-core host) and confirm each display opens on its own after the VDD self-heals — **no VM restart needed**.

Root cause was proven empirically + by code review: the agent samples `devcon` exactly once at connect (`agent.c:1812`) and never refreshes, so an early `error`/`unknown` (VDD's `IddCxSwapChainSetDevice` transiently failing under the concurrent-boot CPU starvation, then self-recovering) stays latched until a reconnect. Windows-on-Windows has the same code but a wider timing margin (native speed), so it's latent there.
