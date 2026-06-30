/*
 * mac_account_hash -- macOS local-account credential encoders, shared by the privileged "stage"
 * disk-applier (tools/iso-patch-mac) and the host daemon (src/backend_mac/iso_patch_mac.m).
 *
 * Sharing them lets the daemon compute the ShadowHash + kcpassword IN-PROCESS and hand the applier
 * only the non-plaintext result -- the same principle the Windows path uses (the password is encoded
 * in the daemon via asb_provision_unattend, never on the disk-applier's command line). The plaintext
 * never crosses the process boundary.
 */
#ifndef ASB_MAC_ACCOUNT_HASH_H
#define ASB_MAC_ACCOUNT_HASH_H

#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

/* Binary-plist value for the dslocal user's ShadowHashData (SALTED-SHA512-PBKDF2: random 32-byte
 * salt, 150000 iterations, 128-byte entropy over the UTF-8 password bytes). Returns nil on failure. */
NSData *_Nullable asb_macos_shadow_hash_data(const char *pw, size_t pwLen);

/* /etc/kcpassword bytes for autologin (the classic 11-byte XOR cipher, padded to a 12-byte boundary). */
NSData *asb_macos_kcpassword(const char *pw, size_t pwLen);

NS_ASSUME_NONNULL_END

#endif /* ASB_MAC_ACCOUNT_HASH_H */
