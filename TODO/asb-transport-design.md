# asb_transport — partitioned shared‑memory channel layer (Windows‑on‑macOS)

Replaces the 8 inlined `socket(AF_HYPERV,SOCK_STREAM,1)` + `bind(SOCKADDR_HV{…, ServiceId=a5b0cafe‑
000N…})` sites with one shared library. **PC build = AF_HYPERV/HCS unchanged** (default). **Mac build
= ivshmem shared memory** via our driver (`tools/ivshmem`). Same byte protocols above the transport
(`hello`/heartbeat/`<seq>:cmd`; `ASIN/ASA1/ACLP/CLDY/IRDY/ASFR/ASCR`). Substrate validated: cached
ivshmem ~28 GB/s, ~3 µs round‑trip, coherent (`[[ivshmem-driver-transport-validated]]`).

> **Status (2026‑06‑21): implemented and in production.** Guest: `tools/transport/asb_transport.{c,h}`
> (PC AF_HYPERV / Mac ivshmem split). Mac host: `src/backend_mac/asb_ivshmem_transport.m` (lays out the
> directory + regions at VM‑create; one pump thread per connection slot). All channels run on the dev
> VM. The SPSC ring carries an **acquire‑barrier** ssh‑integrity fix (`ring_read` / `ring_read_host` —
> order the `tail` load before the data read on weak memory). **Known open gap:** no slot reclaim when a
> guest peer dies without a clean close (agent/VDD restart) — a heartbeat‑based reclaim was implemented
> and **reverted** (it crashed the host and killed the idle ch1 control channel). The sections below
> describe the implemented design; the byte layout is authoritative in `asb_transport.h`.

## Design principle (per user): one dedicated memory region per service
The 128 MiB BAR is **statically partitioned** — every service gets its **own** slice. Each guest
sub‑program is already its own process (`appsandbox-agent/-input/-audio/-clipboard/-clipboard-reader`,
`vdd`, `p9copy`); each runs **its own thread** that reads/writes **only its region**. No service
touches another's memory ⇒ no cross‑service locks, clean parallelism, and a fault in one channel can't
corrupt another. The host (Mac) `mmap`s the same backing file and has a counterpart reader/writer per
region.

## BAR layout (128 MiB default)
```
off 0x00000000  ASB_SHM_DIRECTORY            (one 4 KiB page) — magic/version/region table/liveness
off 0x00001000  ch1  agent control region    256 KiB   1 slot   (host⇄agent: hello/heartbeat/<seq>:cmd)
off 0x00041000  ch3  input region            128 KiB   1 slot   (ASIN/IRDY)
off 0x00061000  ch4  audio region              1 MiB   1 slot   (ASA1 PCM)
off 0x00161000  ch5  clipboard region          2 MiB   1 slot   (ACLP/CLDY, writer)
off 0x00361000  ch6  clipboard‑reader region   2 MiB   1 slot   (CLDY, reader)
off 0x00561000  ch7  ssh region                8 MiB   8 slots  (multiple concurrent sessions)
off 0x00d61000  ch50001 9P region              8 MiB   8 slots  (multiple concurrent file copies)
off 0x01561000  ch2  VDD frame region        ~108 MiB  buffers  (ASFR/ASCR; 4K double/triple‑buffer + cursor)
```
Offsets/sizes are **published in the directory**, not hard‑coded in consumers — so they can change
without recompiling both sides. (Above is the v1 default map.)

## ASB_SHM_DIRECTORY (offset 0)
```c
struct AsbShmDirectory {            // one 4 KiB page, written once by the host at VM create
    uint64_t magic;                 // "ASBSHMD1"
    uint32_t version;               // 1
    uint32_t bar_size;              // 128*1024*1024
    uint64_t host_epoch;            // host liveness (bumped each poll); 0 = host gone
    uint64_t guest_epoch;           // guest agent liveness
    uint32_t n_regions;
    uint32_t _pad;
    struct AsbShmRegionDesc {
        uint32_t channel_id;        // 1,2,3,4,5,6,7,50001
        uint32_t flags;             // ASB_REGION_STREAM | ASB_REGION_FRAME
        uint64_t offset;            // from BAR base
        uint64_t size;
        uint32_t n_slots;           // stream: connection slots (1, or 8 for ssh/9p)
        uint32_t slot_stride;       // stream: bytes per slot
    } regions[ /* n_regions */ ];
};
```
A consumer maps the BAR (driver IOCTL on the guest; `mmap` of the file on the host), finds its
`channel_id` in the table, and uses only `[offset, offset+size)`.

## Stream region — connection slots + SPSC rings
A stream region holds `n_slots` independent connection slots (1 for single‑conn channels; 8 for
ssh/9p). Each slot is a full bidirectional stream = two **single‑producer/single‑consumer** rings:
```c
struct AsbRing {                    // lock-free SPSC; cap is a power of 2
    volatile uint64_t head;         // consumer index (monotonic)
    volatile uint64_t tail;         // producer index (monotonic)
    uint32_t cap;                   // ring data capacity (power of 2)
    uint32_t _pad;
    uint8_t  data[ /* cap */ ];     // bytes; index & (cap-1)
};                                  // used = tail-head; free = cap-(tail-head)
struct AsbSlot {
    volatile uint32_t state;        // FREE, CONNECTING, ESTABLISHED, CLOSING, CLOSED
    uint32_t          _pad;
    AsbRing           g2h;          // guest writes, host reads
    AsbRing           h2g;          // host writes, guest reads
};
```
**Ring discipline (validated pattern):** producer writes payload into `data[tail & (cap-1)] …`,
then `MemoryBarrier(); tail += n` (release). Consumer reads up to `tail-head` bytes from
`data[head & (cap-1)] …`, then `MemoryBarrier(); head += n` (acquire). Cached mapping is coherent on
Apple Silicon, so a full barrier (DMB) on each side is sufficient — no flush. Each ring has exactly
one writer thread and one reader thread ⇒ no locks.

**Connection handshake (maps listen/accept/connect):**
- *Listener* (guest agent/ssh/etc. — the side that did `bind`+`listen`): polls its region's slots for
  one whose `state==CONNECTING`; claims it → `state=ESTABLISHED`. = `accept()`.
- *Connector* (the host, or guest 9P which connects out): finds a `FREE` slot, sets it `CONNECTING`,
  waits for `ESTABLISHED`. = `connect()`.
- *Close*: writer sets `CLOSING`; once both rings drained, `CLOSED`→`FREE`.
- Single‑conn channels (agent/input/audio/clipboard/reader) use slot 0 only.

## Frame region (ch2 VDD)
```c
struct AsbFrameRegion {
    uint32_t magic;                 // 'ASFR'
    uint32_t n_buffers;             // 2 (double) or 3 (triple)
    uint32_t width, height, stride, format;   // BGRA
    volatile uint32_t produced_seq; // VDD bumps after writing a buffer
    volatile uint32_t consumed_seq; // host bumps after reading
    volatile uint32_t active_buffer;// index the VDD just published
    uint32_t dirty_rect_count;      // VDD_WIRE dirty rects follow
    /* cursor: ASCR header + image */
    /* buffer[n_buffers]: each width*height*4 (4K = ~32 MiB) */
};
```
The VDD writes BGRA into the next buffer, sets `active_buffer`, bumps `produced_seq` (release). The
Mac consumer polls `produced_seq`, reads `active_buffer`, uploads to its Metal/`VZ`‑style view, bumps
`consumed_seq`. Double‑buffer minimum so the VDD writes buffer N+1 while the host reads N (tear‑free).
This is the only memcpy‑class channel; it never touches a ring.

## Notification = polling (validated; no doorbell needed yet)
Each side polls its inbound rings/seq. Per‑service threads back off when idle (e.g. 100 µs–1 ms sleep
on the low‑rate control channels; tight spin only on the frame consumer). Round‑trip measured ~3 µs.
A doorbell (eventfd‑style cross‑notify) can be added later if a channel needs lower idle latency, but
ivshmem‑plain has no interrupt — polling is the design.

## The `select()` problem → per‑direction threads (no select on shared memory)
The current code `select()`s the HV socket with a stop‑timeout, and the **ssh relay `select()`s the
HV socket *and* a TCP socket together** — neither works on a ring. The partitioned + polling model
removes `select()` entirely:
- Listener accept = poll slots with a timeout (replaces `select`+`accept`).
- ssh/9p relay per connection = **two threads**: `recv(TCP) → g2h ring` and `h2g ring → send(TCP)`.
  Each is a simple blocking‑TCP + non‑blocking‑ring loop. (Matches "each sub‑program uses its own
  thread" — extended to one pair of threads per live connection.)

## API (`asb_transport.h`) — drop‑in for the socket sites
```c
int            asb_transport_init(void);                 // PC: WSAStartup; Mac: open driver, map BAR, find directory
AsbListener*   asb_listen(int channel);                  // PC: socket+bind(GUID)+listen; Mac: bind region/slots
AsbConn*       asb_accept(AsbListener*, int timeout_ms);  // NULL on timeout (replaces select+accept)
AsbConn*       asb_connect(int channel);                 // 9P connect-out (guest→host)
int            asb_recv(AsbConn*, void* buf, int len);    // blocking; <=0 = closed
int            asb_send(AsbConn*, const void* buf, int len);
void           asb_close(AsbConn*);
void           asb_close_listener(AsbListener*);
/* helpers preserving existing newline framing */
int            asb_recv_line(AsbConn*, char* buf, int n); // == recv_line()
int            asb_send_line(AsbConn*, const char* msg);  // == send_line()
/* VDD frame channel (ch2) */
AsbFrame*      asb_frame_open(int width, int height, int n_buffers);  // VDD side
void*          asb_frame_back_buffer(AsbFrame*);          // back buffer to write the next frame into
void           asb_frame_publish(AsbFrame*);             // mark it produced (bumps produced_seq)
void           asb_frame_close(AsbFrame*);
void*          asb_frame_cursor(AsbFrame*);              // AsbCursor* in the region, or NULL
/* `asb_transport.h` is the authoritative, complete API; it also has: asb_poll, asb_set_timeout,
   asb_abandon, asb_stream_reset, asb_recv_line/asb_send_line, asb_transport_is_ivshmem,
   asb_transport_region_base, asb_ring_drain, asb_conn_socket_u64. */
```
Component change is mechanical: `socket(AF_HYPERV…)`+`bind`+`listen` → `asb_listen(channel)`;
`select`+`accept` → `asb_accept(l, 1000)`; `recv/send` → `asb_recv/asb_send`; `closesocket` →
`asb_close`; channel number replaces the service GUID. Protocols above are untouched.

## Host selection switch (default‑safe; never positively detect "Hyper‑V")
1. Explicit override `HKLM\SOFTWARE\AppSandbox\Transport` / env (Mac builder sets `ivshmem`).
2. Else detect ivshmem: our driver's device interface present
   (`SetupDiGetClassDevs(GUID_DEVINTERFACE_ASB_IVSHMEM)`).
3. Else default **AF_HYPERV** (HCS). PC byte‑identical.
`asb_transport.c` has two backends behind the API; the channel→GUID table is the PC mapping
(`a5b0cafe‑000N‑4000‑8000‑000000000001`), the channel→region table is the Mac mapping.

## Build / placement
`tools/transport/asb_transport.{h,c}` (new shared lib) linked into agent + the 4 channel EXEs +
p9copy + vdd. Mac host counterpart lives in the QEMU backend (`src/backend_mac`), mapping the same
file. No third‑party code (`[[no-external-deps-constraint]]`).
