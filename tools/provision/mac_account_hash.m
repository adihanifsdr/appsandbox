/*
 * mac_account_hash.m -- see mac_account_hash.h. Lifted verbatim from the iso-patch-mac "stage"
 * implementation so both the applier and the daemon produce byte-identical credential material.
 */
#import "mac_account_hash.h"
#import <CommonCrypto/CommonKeyDerivation.h>

NSData *asb_macos_shadow_hash_data(const char *pw, size_t pwLen) {
    unsigned char salt[32];
    arc4random_buf(salt, 32);
    const int iterations = 150000;
    unsigned char entropy[128];
    int rc = CCKeyDerivationPBKDF(kCCPBKDF2, pw, pwLen,
                                   salt, sizeof(salt),
                                   kCCPRFHmacAlgSHA512,
                                   iterations,
                                   entropy, sizeof(entropy));
    if (rc != 0) return nil;

    NSDictionary *inner = @{
        @"SALTED-SHA512-PBKDF2": @{
            @"entropy":    [NSData dataWithBytes:entropy length:sizeof(entropy)],
            @"iterations": @(iterations),
            @"salt":       [NSData dataWithBytes:salt length:sizeof(salt)],
        }
    };
    NSData *plist = [NSPropertyListSerialization dataWithPropertyList:inner
                                                                format:NSPropertyListBinaryFormat_v1_0
                                                               options:0
                                                                 error:NULL];
    memset(entropy, 0, sizeof(entropy));
    memset(salt,    0, sizeof(salt));
    return plist;
}

/* macOS autologin password obfuscation (/etc/kcpassword). */
NSData *asb_macos_kcpassword(const char *pw, size_t pwLen) {
    static const unsigned char cipher[] = {
        0x7D, 0x89, 0x52, 0x23, 0xD2, 0xBC, 0xDD, 0xEA, 0xA3, 0xB9, 0x1F
    };
    size_t outLen = ((pwLen / 12) + 1) * 12; /* round up to next multiple of 12 */
    unsigned char *buf = malloc(outLen);
    for (size_t i = 0; i < outLen; i++) {
        unsigned char src = (i < pwLen) ? (unsigned char)pw[i] : cipher[i % 11];
        buf[i] = src ^ cipher[i % 11];
    }
    NSData *result = [NSData dataWithBytes:buf length:outLen];
    memset(buf, 0, outLen);
    free(buf);
    return result;
}
