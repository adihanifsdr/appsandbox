#import "iso_patch_mac.h"
#import "vm_dir.h"               /* AppSandbox support root (sibling of VMs/) for the driver cache */
#import <Security/Security.h>
#import <Security/AuthorizationTags.h>

/* AuthorizationExecuteWithPrivileges has been deprecated since 10.7 but is
 * still functional. Suppress the warning locally; we'll migrate to
 * SMAppService when ready. */
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"

static AuthorizationRef  g_auth = NULL;
static dispatch_source_t g_authKeepAlive = NULL;

/* Keep-alive interval: shorter than the default 300s TTL of
 * kAuthorizationRightExecute so the right never expires while the app
 * runs. Without this, a long install (>5 min) causes AEWP to prompt a
 * second time when stage finally runs. */
#define AUTH_KEEPALIVE_SECONDS 240

@implementation IsoPatchMac

+ (nullable NSString *)toolPath {
    NSFileManager *fm = [NSFileManager defaultManager];

    /* 1. Bundled next to the host app. */
    NSString *res = [[NSBundle mainBundle].resourcePath
                        stringByAppendingPathComponent:@"iso-patch-mac"];
    if (res && [fm fileExistsAtPath:res]) return res;

    /* 2. Dev fallback: walk up from the bundle to the source tree. */
    NSString *cur = [NSBundle mainBundle].bundlePath;
    for (int i = 0; i < 6 && cur.length > 1; i++) {
        NSString *candidate = [cur stringByAppendingPathComponent:
                                 @"tools/iso-patch-mac/build/iso-patch-mac"];
        if ([fm fileExistsAtPath:candidate]) return candidate;
        cur = [cur stringByDeletingLastPathComponent];
    }
    return nil;
}

#pragma mark - Authorization (for stage)

+ (void)startKeepAlive {
    if (g_authKeepAlive) return;
    g_authKeepAlive = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0,
        dispatch_get_global_queue(QOS_CLASS_UTILITY, 0));
    dispatch_source_set_timer(g_authKeepAlive,
        dispatch_time(DISPATCH_TIME_NOW, AUTH_KEEPALIVE_SECONDS * NSEC_PER_SEC),
        AUTH_KEEPALIVE_SECONDS * NSEC_PER_SEC,
        10 * NSEC_PER_SEC);
    dispatch_source_set_event_handler(g_authKeepAlive, ^{
        if (!g_auth) return;
        /* Non-interactive refresh: if rights are still valid, this silently
         * resets their TTL. If they've somehow expired, we fail silently
         * here and the next real call to ensureAuthorization will prompt. */
        AuthorizationItem right = { kAuthorizationRightExecute, 0, NULL, 0 };
        AuthorizationRights rights = { 1, &right };
        AuthorizationFlags flags = kAuthorizationFlagDefaults
                                 | kAuthorizationFlagExtendRights;
        (void)AuthorizationCopyRights(g_auth, &rights, NULL, flags, NULL);
    });
    /* Free g_auth from the cancel handler: GCD runs it only after any in-flight
     * event handler returns. */
    dispatch_source_set_cancel_handler(g_authKeepAlive, ^{
        if (g_auth) {
            AuthorizationFree(g_auth, kAuthorizationFlagDestroyRights);
            g_auth = NULL;
        }
    });
    dispatch_resume(g_authKeepAlive);
}

+ (BOOL)ensureAuthorization:(NSError **)errOut {
    /* Allocate the ref on first call. */
    if (!g_auth) {
        OSStatus s = AuthorizationCreate(NULL, NULL,
                                          kAuthorizationFlagDefaults, &g_auth);
        if (s != errAuthorizationSuccess) {
            g_auth = NULL;
            if (errOut) *errOut = [NSError errorWithDomain:NSOSStatusErrorDomain code:s
                                                  userInfo:@{NSLocalizedDescriptionKey:
                                                             @"AuthorizationCreate failed"}];
            return NO;
        }
    }

    /* Always re-check rights. If they're still valid (keep-alive kept them
     * fresh), this is silent. If they've expired, this is where the UI
     * prompt fires — which should be at VM-creation time, not mid-install. */
    AuthorizationItem right = { kAuthorizationRightExecute, 0, NULL, 0 };
    AuthorizationRights rights = { 1, &right };
    AuthorizationFlags flags = kAuthorizationFlagDefaults
                             | kAuthorizationFlagInteractionAllowed
                             | kAuthorizationFlagPreAuthorize
                             | kAuthorizationFlagExtendRights;
    OSStatus s = AuthorizationCopyRights(g_auth, &rights, NULL, flags, NULL);
    if (s != errAuthorizationSuccess) {
        if (errOut) *errOut = [NSError errorWithDomain:NSOSStatusErrorDomain code:s
                                              userInfo:@{NSLocalizedDescriptionKey:
                                                         @"User denied admin prompt"}];
        return NO;
    }

    [self startKeepAlive];
    return YES;
}

+ (void)releaseAuthorization {
    if (g_authKeepAlive) {
        /* The cancel handler frees g_auth; don't free it here as well. */
        dispatch_source_cancel(g_authKeepAlive);
        g_authKeepAlive = NULL;
        return;
    }
    /* Keep-alive was never started (auth created but rights check failed, so
     * no timer/handler ever existed); free directly. */
    if (g_auth) {
        AuthorizationFree(g_auth, kAuthorizationFlagDestroyRights);
        g_auth = NULL;
    }
}

+ (BOOL)preauthorize:(NSError **)error {
    /* Already root (the headless daemon runs under sudo): no AuthorizationRef
     * needed -- runPrivilegedArgs launches the stage step directly. Keeps
     * headless fully non-interactive (no username/password prompt). */
    if (geteuid() == 0) return YES;
    return [self ensureAuthorization:error];
}

#pragma mark - Stdout protocol parser

/* Parses a single line from the CLI's stdout; updates progress/final state. */
+ (void)parseLine:(NSString *)line
         progress:(IsoPatchProgress)progressBlock
        finalPath:(NSString **)finalPath
        finalErr:(NSString **)finalErr {
    if ([line hasPrefix:@"STATUS:"]) {
        NSString *s = [line substringFromIndex:7];
        if (progressBlock) {
            dispatch_async(dispatch_get_main_queue(), ^{ progressBlock(-1.0, s); });
        }
        return;
    }
    if ([line hasPrefix:@"PROGRESS:"]) {
        NSString *rest = [line substringFromIndex:9];
        NSRange colon = [rest rangeOfString:@":"];
        int pct = (int)[rest intValue];
        NSString *step = (colon.location != NSNotFound)
            ? [rest substringFromIndex:colon.location + 1] : @"";
        double frac = (double)pct / 100.0;
        if (progressBlock) {
            dispatch_async(dispatch_get_main_queue(), ^{ progressBlock(frac, step); });
        }
        return;
    }
    if ([line hasPrefix:@"LOG:"]) {
        NSLog(@"iso-patch-mac: %@", [line substringFromIndex:4]);
        return;
    }
    if ([line hasPrefix:@"DONE:"]) {
        if (finalPath) *finalPath = [line substringFromIndex:5];
        return;
    }
    if ([line hasPrefix:@"ERROR:"]) {
        if (finalErr) *finalErr = [line substringFromIndex:6];
        return;
    }
}

/* Consumes the pipe, calling parseLine: for each complete line. Fires
 * `completion` on the main queue when EOF is reached. */
+ (void)drainFileHandle:(NSFileHandle *)fh
                 pipeFP:(FILE *)pipeFP
                progress:(IsoPatchProgress)progressBlock
              completion:(IsoPatchCompletion)completion {
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
        __block NSString *finalPath = nil;
        __block NSString *finalErr  = nil;

        NSMutableData *buf = [NSMutableData data];
        while (true) {
            NSData *chunk = nil;
            if (fh) {
                chunk = [fh availableData];
                if (chunk.length == 0) break;
            } else if (pipeFP) {
                char cbuf[4096];
                size_t r = fread(cbuf, 1, sizeof(cbuf), pipeFP);
                if (r == 0) break;
                chunk = [NSData dataWithBytes:cbuf length:r];
            } else {
                break;
            }
            [buf appendData:chunk];

            const char *bytes = buf.bytes;
            NSUInteger len = buf.length;
            NSUInteger start = 0;
            for (NSUInteger i = 0; i < len; i++) {
                if (bytes[i] != '\n') continue;
                NSString *line = [[NSString alloc] initWithBytes:bytes + start
                                                           length:i - start
                                                         encoding:NSUTF8StringEncoding];
                if (line.length) {
                    [self parseLine:line
                           progress:progressBlock
                          finalPath:&finalPath
                          finalErr:&finalErr];
                }
                start = i + 1;
            }
            if (start > 0) {
                [buf replaceBytesInRange:NSMakeRange(0, start) withBytes:NULL length:0];
            }
        }
        if (pipeFP) fclose(pipeFP);

        NSError *err = nil;
        if (finalErr) {
            err = [NSError errorWithDomain:@"IsoPatchMac" code:2
                     userInfo:@{NSLocalizedDescriptionKey: finalErr}];
        } else if (!finalPath) {
            err = [NSError errorWithDomain:@"IsoPatchMac" code:3
                     userInfo:@{NSLocalizedDescriptionKey:
                                @"iso-patch-mac exited without DONE or ERROR"}];
        }
        dispatch_async(dispatch_get_main_queue(), ^{ completion(err); });
    });
}

#pragma mark - Unprivileged (NSTask)

/* Returns the launched task, or nil on failure (completion fired with error). */
+ (nullable NSTask *)runUnprivilegedArgs:(NSArray<NSString *> *)args
                                progress:(IsoPatchProgress)progressBlock
                              completion:(IsoPatchCompletion)completion {
    NSString *tool = [self toolPath];
    if (!tool) {
        completion([NSError errorWithDomain:@"IsoPatchMac" code:1
                     userInfo:@{NSLocalizedDescriptionKey:
                                @"iso-patch-mac binary not found"}]);
        return nil;
    }

    NSTask *task = [[NSTask alloc] init];
    task.launchPath = tool;
    task.arguments  = args;
    NSPipe *outPipe = [NSPipe pipe];
    task.standardOutput = outPipe;
    task.standardError  = outPipe; /* merge so errors show up in our parser */

    NSError *launchErr = nil;
    if (![task launchAndReturnError:&launchErr]) {
        completion(launchErr);
        return nil;
    }

    [self drainFileHandle:outPipe.fileHandleForReading
                   pipeFP:NULL
                 progress:progressBlock
               completion:completion];
    return task;
}

#pragma mark - Privileged (AEWP)

+ (void)runPrivilegedArgs:(NSArray<NSString *> *)args
                  progress:(IsoPatchProgress)progressBlock
                completion:(IsoPatchCompletion)completion {
    NSString *tool = [self toolPath];
    if (!tool) {
        completion([NSError errorWithDomain:@"IsoPatchMac" code:1
                     userInfo:@{NSLocalizedDescriptionKey:
                                @"iso-patch-mac binary not found"}]);
        return;
    }

    /* Already root (headless daemon under sudo): a plain NSTask child IS
     * privileged -- skip AuthorizationExecuteWithPrivileges entirely, so no
     * interactive prompt can ever appear in headless mode. */
    if (geteuid() == 0) {
        [self runUnprivilegedArgs:args progress:progressBlock completion:completion];
        return;
    }

    NSError *authErr = nil;
    if (![self ensureAuthorization:&authErr]) {
        completion(authErr);
        return;
    }

    NSUInteger n = args.count;
    char **cargs = calloc(n + 1, sizeof(char *));
    for (NSUInteger i = 0; i < n; i++) cargs[i] = strdup([args[i] UTF8String]);
    cargs[n] = NULL;

    FILE *pipe = NULL;
    OSStatus s = AuthorizationExecuteWithPrivileges(g_auth,
        tool.UTF8String,
        kAuthorizationFlagDefaults,
        cargs,
        &pipe);

    for (NSUInteger i = 0; i < n; i++) free(cargs[i]);
    free(cargs);

    if (s != errAuthorizationSuccess) {
        completion([NSError errorWithDomain:NSOSStatusErrorDomain code:s
                     userInfo:@{NSLocalizedDescriptionKey:
                                @"AuthorizationExecuteWithPrivileges failed"}]);
        return;
    }

    [self drainFileHandle:nil
                   pipeFP:pipe
                 progress:progressBlock
               completion:completion];
}

#pragma mark - Public API

/* Keyed by VM name. Mutated only on s_fetchQueue. */
static NSMutableDictionary<NSString *, NSTask *> *s_fetchTasks = nil;
static dispatch_queue_t s_fetchQueue = NULL;

static void ensure_fetch_registry(void) {
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        s_fetchTasks = [NSMutableDictionary dictionary];
        s_fetchQueue = dispatch_queue_create("com.appsandbox.iso-patch.fetch",
                                             DISPATCH_QUEUE_SERIAL);
    });
}

+ (void)fetchLatestIpswToURL:(NSURL *)ipswURL
                        forVm:(NSString *)vmName
                     progress:(IsoPatchProgress)progressBlock
                   completion:(IsoPatchCompletion)completion {
    ensure_fetch_registry();

    NSString *key = [vmName copy];
    IsoPatchCompletion wrapped = ^(NSError *err) {
        dispatch_async(s_fetchQueue, ^{ [s_fetchTasks removeObjectForKey:key]; });
        if (completion) completion(err);
    };

    NSTask *task = [self runUnprivilegedArgs:@[@"fetch-ipsw", @"--output", ipswURL.path]
                                     progress:progressBlock
                                   completion:wrapped];
    if (task) {
        dispatch_async(s_fetchQueue, ^{ s_fetchTasks[key] = task; });
    }
}

+ (void)cancelFetchForVm:(NSString *)vmName {
    ensure_fetch_registry();
    NSString *key = [vmName copy];
    dispatch_async(s_fetchQueue, ^{
        NSTask *task = s_fetchTasks[key];
        if (!task) return;
        [s_fetchTasks removeObjectForKey:key];
        if (task.isRunning) {
            @try { [task terminate]; }
            @catch (NSException *e) { (void)e; }
        }
    });
}

+ (void)installMacOSWithName:(NSString *)name
                       vmDir:(NSURL *)vmDir
                    ipswURL:(NSURL *)ipswURL
                       ramMb:(int)ramMb
                       cpus:(int)cpus
                      diskGb:(int)diskGb
                   progress:(IsoPatchProgress)progressBlock
                 completion:(IsoPatchCompletion)completion {
    NSArray *args = @[
        @"install",
        @"--name",     name,
        @"--vm-dir",   vmDir.path,
        @"--ipsw",     ipswURL.path,
        @"--ram-mb",   [NSString stringWithFormat:@"%d", ramMb],
        @"--cpus",     [NSString stringWithFormat:@"%d", cpus],
        @"--disk-gb",  [NSString stringWithFormat:@"%d", diskGb],
    ];
    [self runUnprivilegedArgs:args
                     progress:progressBlock
                   completion:completion];
}

#pragma mark - build-windows (unprivileged)

+ (void)buildWindowsDiskWithISO:(NSURL *)isoURL
                        outDisk:(NSURL *)outDiskURL
               signedPayloadZip:(NSString *)signedPayloadZip
                      netkvmZip:(nullable NSString *)netkvmZip
                     sshMsiPath:(nullable NSString *)sshMsiPath
                         vmName:(NSString *)vmName
                      adminUser:(NSString *)adminUser
                      adminPass:(NSString *)adminPass
                           lang:(nullable NSString *)lang
                         diskGb:(int)diskGb
                       testMode:(BOOL)testMode
                       progress:(nullable IsoPatchProgress)progressBlock
                     completion:(IsoPatchCompletion)completion {
    NSMutableArray *args = [@[
        @"build-windows",
        @"--iso",     isoURL.path,
        @"--out",     outDiskURL.path,
        @"--signed-payload-zip", signedPayloadZip,
        @"--vm-name", vmName,
        @"--user",    adminUser,
        @"--pass",    adminPass,
        @"--lang",    (lang.length ? lang : @"en-US"),
        @"--disk-gb", [NSString stringWithFormat:@"%d", diskGb > 0 ? diskGb : 64],
        @"--test-mode", (testMode ? @"1" : @"0"),   /* honor the create-time choice (mirrors Windows) */
    ] mutableCopy];
    if (netkvmZip.length)  { [args addObject:@"--netkvm-zip"]; [args addObject:netkvmZip]; }
    if (sshMsiPath.length) { [args addObject:@"--ssh-msi"];    [args addObject:sshMsiPath]; }
    [self runUnprivilegedArgs:args
                     progress:progressBlock
                   completion:completion];
}

/* Mirror of ensure_ssh_msi_cached: download the OpenSSH ARM64 MSI once, cache it under
 * <Caches>/AppSandbox/, return the cached path (or nil). Blocking; the build flow runs off-main. */
+ (NSString *)ensureOpenSSHMsiCached {
    NSString *name = @"OpenSSH-ARM64-v10.0.0.0.msi";
    NSString *urlStr = @"https://github.com/PowerShell/Win32-OpenSSH/releases/download/"
                        "10.0.0.0p2-Preview/OpenSSH-ARM64-v10.0.0.0.msi";
    /* Cache under the AppSandbox support dir (sibling of VMs/ + restore.ipsw), NOT ~/Library/Caches —
     * VmDir's root is sudo-aware, so the daemon (root) and the GUI (user) share one cache. */
    NSString *cacheDir = [[VmDir vmsRootDirectory] URLByDeletingLastPathComponent].path;
    [[NSFileManager defaultManager] createDirectoryAtPath:cacheDir
                              withIntermediateDirectories:YES attributes:nil error:nil];
    NSString *dst = [cacheDir stringByAppendingPathComponent:name];
    if ([[NSFileManager defaultManager] fileExistsAtPath:dst]) return dst;

    /* Synchronous download (follows the GitHub->CDN redirect). */
    __block NSData *data = nil;
    dispatch_semaphore_t sem = dispatch_semaphore_create(0);
    NSURLSessionDataTask *t = [[NSURLSession sharedSession]
        dataTaskWithURL:[NSURL URLWithString:urlStr]
      completionHandler:^(NSData *d, NSURLResponse *resp, NSError *err) {
        NSInteger code = [resp isKindOfClass:[NSHTTPURLResponse class]] ? ((NSHTTPURLResponse *)resp).statusCode : 0;
        if (d.length && (code == 200 || code == 0) && !err) data = d;
        dispatch_semaphore_signal(sem);
    }];
    [t resume];
    dispatch_semaphore_wait(sem, dispatch_time(DISPATCH_TIME_NOW, (int64_t)180 * NSEC_PER_SEC));
    if (data.length && [data writeToFile:dst atomically:YES]) return dst;
    return nil;
}

/* See iso_patch_mac.h. Cache the signed Windows release zip. Mirrors ensureOpenSSHMsiCached: a single
 * atomic write to a version-named file under the AppSandbox support dir (sibling of VMs/), shared by
 * the root daemon + GUI. build-windows extracts the needed members into its own per-build temp. */
+ (nullable NSString *)ensureSignedWinPayloadZipCached {
    /* Signed ARM64 release: EV-signed agent EXEs + MS-attestation-signed drivers (VDD/VAD/AppSandboxSHM/
     * devcon). The version is pinned in the cached filename, so a newer release auto-invalidates. The
     * URL must stay publicly fetchable by an unauthenticated client. */
    NSString *name   = @"AppSandbox-0.1.3-win-arm64.zip";
    NSString *urlStr = @"https://github.com/user-attachments/files/29446145/AppSandbox-0.1.3-win-arm64.zip";
    NSString *cacheDir = [[VmDir vmsRootDirectory] URLByDeletingLastPathComponent].path;
    [[NSFileManager defaultManager] createDirectoryAtPath:cacheDir
                              withIntermediateDirectories:YES attributes:nil error:nil];
    NSString *dst = [cacheDir stringByAppendingPathComponent:name];
    if ([[NSFileManager defaultManager] fileExistsAtPath:dst]) return dst;   /* cache hit */

    /* Synchronous download (follows the GitHub->CDN redirect). The atomic writeToFile makes concurrent
     * creates race-safe (worst case: a redundant download), exactly like the OpenSSH MSI. */
    __block NSData *data = nil;
    dispatch_semaphore_t sem = dispatch_semaphore_create(0);
    NSURLSessionDataTask *t = [[NSURLSession sharedSession]
        dataTaskWithURL:[NSURL URLWithString:urlStr]
      completionHandler:^(NSData *d, NSURLResponse *resp, NSError *err) {
        NSInteger code = [resp isKindOfClass:[NSHTTPURLResponse class]] ? ((NSHTTPURLResponse *)resp).statusCode : 0;
        if (d.length && (code == 200 || code == 0) && !err) data = d;
        dispatch_semaphore_signal(sem);
    }];
    [t resume];
    if (dispatch_semaphore_wait(sem, dispatch_time(DISPATCH_TIME_NOW, (int64_t)300 * NSEC_PER_SEC)) != 0)
        [t cancel];   /* timeout: don't leak the in-flight task */
    if (data.length && [data writeToFile:dst atomically:YES]) return dst;
    return nil;
}

/* See iso_patch_mac.h. Cache the vendored NetKVM zip. Mirrors ensureSignedWinPayloadZipCached: a single
 * atomic write to a fixed-named file under the AppSandbox support dir (sibling of VMs/), shared by the
 * root daemon + GUI. Pulled from the public repo (raw.githubusercontent.com); the file never changes. */
+ (nullable NSString *)ensureNetkvmZipCached {
    NSString *name   = @"netkvm-arm64.zip";
    NSString *urlStr = @"https://raw.githubusercontent.com/jamesstringer90/appsandbox/win-on-mac/vendor/virtio-win/netkvm-arm64.zip";
    NSString *cacheDir = [[VmDir vmsRootDirectory] URLByDeletingLastPathComponent].path;
    [[NSFileManager defaultManager] createDirectoryAtPath:cacheDir
                              withIntermediateDirectories:YES attributes:nil error:nil];
    NSString *dst = [cacheDir stringByAppendingPathComponent:name];
    if ([[NSFileManager defaultManager] fileExistsAtPath:dst]) return dst;   /* cache hit */

    __block NSData *data = nil;
    dispatch_semaphore_t sem = dispatch_semaphore_create(0);
    NSURLSessionDataTask *t = [[NSURLSession sharedSession]
        dataTaskWithURL:[NSURL URLWithString:urlStr]
      completionHandler:^(NSData *d, NSURLResponse *resp, NSError *err) {
        NSInteger code = [resp isKindOfClass:[NSHTTPURLResponse class]] ? ((NSHTTPURLResponse *)resp).statusCode : 0;
        if (d.length && (code == 200 || code == 0) && !err) data = d;
        dispatch_semaphore_signal(sem);
    }];
    [t resume];
    if (dispatch_semaphore_wait(sem, dispatch_time(DISPATCH_TIME_NOW, (int64_t)180 * NSEC_PER_SEC)) != 0)
        [t cancel];
    if (data.length && [data writeToFile:dst atomically:YES]) return dst;
    return nil;
}

#pragma mark - stage (privileged)

+ (NSString *)formatManifestForAgentDir:(NSString *)dir
                              binarySrc:(NSString *)binPath {
    /* Columns: src <TAB> dest_rel <TAB> mode_octal <TAB> owner
     * Clipboard binary + plist are staged into /Library/AppSandbox/;
     * firstboot.sh moves the plist into /Library/LaunchAgents/ on first
     * boot (same root-chown dance as the agent LaunchDaemon). */
    NSFileManager *fm = [NSFileManager defaultManager];
    NSString *clipBin = [dir stringByAppendingPathComponent:@"appsandbox-clipboard"];
    if (![fm fileExistsAtPath:clipBin])
        clipBin = [dir stringByAppendingPathComponent:@"build/appsandbox-clipboard"];
    BOOL haveClip      = [fm fileExistsAtPath:clipBin];
    NSString *clipPlist = [dir stringByAppendingPathComponent:@"com.appsandbox.clipboard.plist"];
    BOOL haveClipPlist = [fm fileExistsAtPath:clipPlist];

    NSMutableString *m = [NSMutableString stringWithFormat:
        @"%@\tLibrary/AppSandbox/appsandbox-agent\t0755\troot:wheel\n"
        @"%@/com.appsandbox.agent.plist\tLibrary/AppSandbox/com.appsandbox.agent.plist\t0644\troot:wheel\n"
        @"%@/firstboot.sh\tLibrary/AppSandbox/firstboot.sh\t0755\troot:wheel\n"
        @"%@/com.appsandbox.firstboot.plist\tLibrary/LaunchDaemons/com.appsandbox.firstboot.plist\t0644\troot:wheel\n",
        binPath, dir, dir, dir];
    if (haveClip) {
        [m appendFormat:@"%@\tLibrary/AppSandbox/appsandbox-clipboard\t0755\troot:wheel\n",
            clipBin];
    }
    if (haveClipPlist) {
        [m appendFormat:@"%@\tLibrary/AppSandbox/com.appsandbox.clipboard.plist\t0644\troot:wheel\n",
            clipPlist];
    }
    return m;
}

+ (void)stageAgentIntoDiskAtURL:(NSURL *)diskURL
               agentResourceDir:(NSString *)agentResDir
                       adminUser:(NSString *)adminUser
                       adminPass:(NSString *)adminPass
                    computerName:(NSString *)computerName
                     sshEnabled:(BOOL)sshEnabled
                       progress:(IsoPatchProgress)progressBlock
                     completion:(IsoPatchCompletion)completion {
    NSFileManager *fm = [NSFileManager defaultManager];

    NSString *binPath = [agentResDir stringByAppendingPathComponent:@"appsandbox-agent"];
    if (![fm fileExistsAtPath:binPath]) {
        binPath = [agentResDir stringByAppendingPathComponent:@"build/appsandbox-agent"];
    }
    if (![fm fileExistsAtPath:binPath]) {
        completion([NSError errorWithDomain:@"IsoPatchMac" code:4
                     userInfo:@{NSLocalizedDescriptionKey:
                                @"appsandbox-agent binary not found"}]);
        return;
    }

    for (NSString *name in @[@"com.appsandbox.agent.plist",
                             @"com.appsandbox.firstboot.plist",
                             @"firstboot.sh"]) {
        if (![fm fileExistsAtPath:[agentResDir stringByAppendingPathComponent:name]]) {
            completion([NSError errorWithDomain:@"IsoPatchMac" code:5
                         userInfo:@{NSLocalizedDescriptionKey:
                                    [NSString stringWithFormat:@"missing %@", name]}]);
            return;
        }
    }

    /* Write manifest + password to temp files (both mode 0600). Password
     * goes to its own file — we don't pass it as a command-line arg so `ps`
     * can't see it. Files are deleted as soon as the CLI exits. */
    NSString *manifest = [self formatManifestForAgentDir:agentResDir binarySrc:binPath];
    NSString *manifestPath = [NSTemporaryDirectory() stringByAppendingPathComponent:
                                [NSString stringWithFormat:@"iso-patch-mac-%u.tsv", arc4random()]];
    NSString *passPath = [NSTemporaryDirectory() stringByAppendingPathComponent:
                            [NSString stringWithFormat:@"iso-patch-mac-%u.pass", arc4random()]];

    NSError *wErr = nil;
    if (![manifest writeToFile:manifestPath atomically:YES
                      encoding:NSUTF8StringEncoding error:&wErr]) {
        completion(wErr); return;
    }
    [[NSFileManager defaultManager] setAttributes:@{NSFilePosixPermissions: @(0600)}
                                     ofItemAtPath:manifestPath error:nil];

    NSData *passData = [adminPass dataUsingEncoding:NSUTF8StringEncoding];
    if (![passData writeToFile:passPath atomically:YES]) {
        [fm removeItemAtPath:manifestPath error:nil];
        completion([NSError errorWithDomain:@"IsoPatchMac" code:6
                     userInfo:@{NSLocalizedDescriptionKey:
                                @"failed to write password tempfile"}]);
        return;
    }
    [[NSFileManager defaultManager] setAttributes:@{NSFilePosixPermissions: @(0600)}
                                     ofItemAtPath:passPath error:nil];

    NSMutableArray *args = [@[
        @"stage",
        @"--disk", diskURL.path,
        @"--manifest", manifestPath,
        @"--user-shortname", adminUser,
        @"--user-realname", adminUser,
        @"--user-uid", @"501",
        @"--user-password-file", passPath,
        @"--skip-setup-assistant",
        @"--auto-login",
    ] mutableCopy];
    if (sshEnabled) [args addObject:@"--enable-ssh"];
    if (computerName.length) {
        [args addObject:@"--computer-name"];
        [args addObject:computerName];
    }

    [self runPrivilegedArgs:args
                   progress:progressBlock
                 completion:^(NSError * _Nullable err) {
        /* Zero + remove tempfiles ASAP. */
        int pfd = open(passPath.fileSystemRepresentation, O_WRONLY);
        if (pfd >= 0) {
            char zeros[256] = {0};
            write(pfd, zeros, sizeof(zeros));
            close(pfd);
        }
        [[NSFileManager defaultManager] removeItemAtPath:manifestPath error:nil];
        [[NSFileManager defaultManager] removeItemAtPath:passPath error:nil];
        completion(err);
    }];
}

@end

#pragma clang diagnostic pop
