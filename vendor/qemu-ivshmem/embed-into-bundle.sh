#!/usr/bin/env bash
# Run by the Xcode "Embed QEMU" build phase. Copies the vendored QEMU + EDK2 payload
# (vendor/qemu-ivshmem/dist) into the app bundle's Resources/qemu.
#   - binary + dylibs: decompressed if stored .gz, else copied (they must run/load)
#   - firmware edk2-*.fd.gz: copied verbatim (the app decompresses it at VM-create)
# Reads SRCROOT / TARGET_BUILD_DIR / UNLOCALIZED_RESOURCES_FOLDER_PATH from Xcode's env.
set -e
SRC="$SRCROOT/vendor/qemu-ivshmem/dist"
DST="$TARGET_BUILD_DIR/$UNLOCALIZED_RESOURCES_FOLDER_PATH/qemu"
if [ ! -d "$SRC" ]; then echo "warning: $SRC not found; QEMU not embedded"; exit 0; fi

rm -rf "$DST"
mkdir -p "$DST/bin" "$DST/lib" "$DST/share/qemu"

if [ -f "$SRC/bin/qemu-system-aarch64.gz" ]; then
  gunzip -c "$SRC/bin/qemu-system-aarch64.gz" > "$DST/bin/qemu-system-aarch64"
else
  cp "$SRC/bin/qemu-system-aarch64" "$DST/bin/qemu-system-aarch64"
fi
chmod +x "$DST/bin/qemu-system-aarch64"

for f in "$SRC"/lib/*.dylib.gz; do [ -e "$f" ] || continue; gunzip -c "$f" > "$DST/lib/$(basename "${f%.gz}")"; done
for f in "$SRC"/lib/*.dylib;    do [ -e "$f" ] || continue; cp "$f" "$DST/lib/"; done

# Ad-hoc sign qemu + dylibs here. Xcode does NOT auto-sign Mach-O placed under Resources/, and this
# build script phase runs in a sandbox that can't reach the signing keychain (Developer-ID signing
# from a script phase fails "no identity found"). So we ad-hoc sign for a valid dev build; the
# Developer-ID re-sign + hardened runtime + timestamp required for notarization is done later, with
# keychain access, by tools/sign/make-release-mac.sh. The qemu binary carries the HVF entitlement.
SIGN=(codesign --force --sign -)
for d in "$DST"/lib/*.dylib; do [ -e "$d" ] && "${SIGN[@]}" "$d"; done
"${SIGN[@]}" --entitlements "$SRCROOT/vendor/qemu-ivshmem/qemu.entitlements" "$DST/bin/qemu-system-aarch64"

cp "$SRC"/share/qemu/*.gz "$DST/share/qemu/" 2>/dev/null || true

# License notices + written offer + version manifest travel in the bundle. The actual source
# tarballs stay in the repo's source-mirror/ (referenced by the written offer), NOT bundled —
# keeps the app small while staying GPL/LGPL compliant.
VEN="$SRCROOT/vendor/qemu-ivshmem"
[ -d "$VEN/LICENSES" ]          && { rm -rf "$DST/LICENSES"; cp -R "$VEN/LICENSES" "$DST/LICENSES"; }
[ -f "$VEN/WRITTEN_OFFER.txt" ] && cp "$VEN/WRITTEN_OFFER.txt" "$DST/WRITTEN_OFFER.txt"
[ -f "$SRC/VERSIONS.txt" ]      && cp "$SRC/VERSIONS.txt" "$DST/VERSIONS.txt"

echo "Embedded QEMU -> $DST ($(du -sh "$DST" | awk '{print $1}'))"
