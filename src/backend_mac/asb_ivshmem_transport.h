/*
 * asb_ivshmem_transport.h -- macOS host-side ivshmem transport.
 *
 * For a Windows guest (QEMU + ivshmem), the host talks to the guest's agent and channel helpers
 * over shared-memory ring slots instead of a VZVirtioSocketDevice. This object maps the
 * launcher-published backing file and, per channel, hands the existing helpers
 * (VmAgentMac / VmSshProxyMac / VmClipboardMac) a connected blocking fd — so their protocol code
 * (recv/send/select) is identical whether the guest is reached via VZ vsock or ivshmem.
 *
 * Role: the host is the CONNECTOR (it arms a slot CONNECTING; the guest's asb_accept establishes
 * it) for the agent/clipboard/ssh channels, mirroring connectToPort: on the VZ path.
 */
#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

@interface AsbIvshmemTransport : NSObject

/* Map the backing file the launcher already published the directory into (ASB_SHM_DIR_MAGIC must be
 * present). Returns nil if the file can't be mapped or the directory isn't published yet. */
- (nullable instancetype)initWithBackingPath:(NSString *)path;

/* Claim + arm a slot for `channel` (ASB_CH_*), wait up to timeoutMs for the guest to accept, then
 * return a blocking AF_UNIX fd bridged to the slot's rings by a background pump thread. The caller
 * owns the fd and uses plain send()/recv()/select() on it; closing it tears the bridge down and
 * releases the slot. Returns -1 on failure/timeout. For multi-slot channels (ssh) each call claims
 * the next free slot. */
- (int)connectChannel:(int)channel timeoutMs:(int)timeoutMs;

/* Stop all pumps and unmap. Idempotent. */
- (void)close;

@property (nonatomic, readonly) BOOL mapped;

@end

NS_ASSUME_NONNULL_END
