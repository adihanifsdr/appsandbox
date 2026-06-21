/*
 * iso_patch_mac -- Host-side wrapper around the iso-patch-mac CLI.
 *
 * Mirrors backend_win's spawn-and-parse integration with tools/iso-patch
 * on Windows. The CLI owns the whole disk-build pipeline; this wrapper
 * drives it via NSTask for the unprivileged steps (fetch-ipsw, install)
 * and via AuthorizationExecuteWithPrivileges for the privileged step
 * (stage). The user is prompted once per host session for the stage
 * step; subsequent stage calls reuse the cached AuthorizationRef.
 */

#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

typedef void (^IsoPatchProgress)(double fraction, NSString *step);
typedef void (^IsoPatchCompletion)(NSError * _Nullable error);

@interface IsoPatchMac : NSObject

/* Path to the bundled iso-patch-mac binary; nil if not located. */
+ (nullable NSString *)toolPath;

/* Acquire the AuthorizationRef needed by stageAgentIntoDiskAtURL: ahead of
 * time. Call this at the start of VM creation so the user is prompted once,
 * immediately, rather than 20 minutes into the install. Returns NO if the
 * user cancels. The ref is cached for the process lifetime. */
+ (BOOL)preauthorize:(NSError * _Nullable * _Nullable)error;

/* Download the latest supported macOS restore image to ipswURL (unprivileged). */
+ (void)fetchLatestIpswToURL:(NSURL *)ipswURL
                        forVm:(NSString *)vmName
                     progress:(nullable IsoPatchProgress)progress
                   completion:(IsoPatchCompletion)completion;

/* Terminate any in-flight fetch for vmName. */
+ (void)cancelFetchForVm:(NSString *)vmName;

/* Create disk.img/aux/hwmodel/machine-id in vmDir and run VZMacOSInstaller
 * (unprivileged). */
+ (void)installMacOSWithName:(NSString *)name
                       vmDir:(NSURL *)vmDir
                    ipswURL:(NSURL *)ipswURL
                       ramMb:(int)ramMb
                       cpus:(int)cpus
                      diskGb:(int)diskGb
                   progress:(nullable IsoPatchProgress)progress
                 completion:(IsoPatchCompletion)completion;

/* Stage the agent bundle + inject a pre-created admin user + skip Setup
 * Assistant + enable auto-login, all in one privileged (AEWP) mount cycle.
 * Pass sshEnabled=YES to also flip sshd's launchd override so the SSH
 * proxy can reach 127.0.0.1:22 on first boot. computerName is the free-form
 * display name shown inside the guest (Sharing prefs, hostname, Bonjour). */
+ (void)stageAgentIntoDiskAtURL:(NSURL *)diskURL
               agentResourceDir:(NSString *)agentResDir
                       adminUser:(NSString *)adminUser
                       adminPass:(NSString *)adminPass
                    computerName:(NSString *)computerName
                     sshEnabled:(BOOL)sshEnabled
                       progress:(nullable IsoPatchProgress)progress
                     completion:(IsoPatchCompletion)completion;

/* Build a from-scratch Windows VM disk (unprivileged): mount the ISO, apply
 * install.wim with our own NTFS writer, and stage the answer file + agent +
 * test-signed drivers. Mirrors the Windows VHDX-first create path. payloadDir
 * holds the guest EXEs + a drivers/ subdir; devconExe is the ARM64 devcon that
 * installs the root-enumerated VDD/VAD devnodes (may be nil). */
+ (void)buildWindowsDiskWithISO:(NSURL *)isoURL
                        outDisk:(NSURL *)outDiskURL
                     payloadDir:(NSString *)payloadDir
                      devconExe:(nullable NSString *)devconExe
                     sshMsiPath:(nullable NSString *)sshMsiPath
                         vmName:(NSString *)vmName
                      adminUser:(NSString *)adminUser
                      adminPass:(NSString *)adminPass
                           lang:(nullable NSString *)lang
                         diskGb:(int)diskGb
                       testMode:(BOOL)testMode
                       progress:(nullable IsoPatchProgress)progress
                     completion:(IsoPatchCompletion)completion;

/* Ensure the OpenSSH ARM64 MSI is cached locally (downloads it on first use, mirroring the Windows
 * ensure_ssh_msi_cached). Returns the cached path, or nil on failure. Blocking; call off-main.
 * (NetKVM/virtio-net is NOT downloaded — it is vendored in the payload's drivers/ dir.) */
+ (nullable NSString *)ensureOpenSSHMsiCached;

/* Free cached AuthorizationRef. Call from asb_mac_cleanup. */
+ (void)releaseAuthorization;

@end

NS_ASSUME_NONNULL_END
