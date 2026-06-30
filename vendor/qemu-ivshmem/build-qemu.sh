#!/usr/bin/env bash
# Build our patched QEMU (ivshmem-plain on macOS/HVF) from the AppSandbox fork.
#
# Source of truth: github.com/jamesstringer90/asb-qemu @ branch asb-ivshmem-11.0.1
#   = upstream tag v11.0.1 + 3 ivshmem commits. The fork carries the ivshmem changes
#     as commits, so this script just clones it and applies no local patches.
# This fork IS our GPL "corresponding source": it builds the exact binary we ship.
#
# Output:     vendor/qemu-ivshmem/qemu-build/qemu/bin/qemu-system-aarch64-ivshmem
# Clone+tree: vendor/qemu-ivshmem/qemu-build/qemu-src (git-ignored; reproducible).
# Everything stays INSIDE appsandbox — nothing is written or read outside the repo.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"          # appsandbox/vendor/qemu-ivshmem
BUILD="$HERE/qemu-build"                        # clone + ninja build tree + output binary
SRC="$BUILD/qemu-src"
OUT="$BUILD/qemu"
FORK="https://github.com/jamesstringer90/asb-qemu"
BRANCH="asb-ivshmem-11.0.1"
export PATH="/opt/homebrew/bin:/usr/local/bin:$PATH"

# Build-time tools only (NOT linked into the shipped binary). glib is the one real
# runtime dep; pixman/slirp/etc. are configured out below.
echo "==> build-time tools"; brew install meson ninja pkg-config glib >/dev/null 2>&1 || true

echo "==> fetch fork ($BRANCH)"
mkdir -p "$SRC"
if [ -d "$SRC/asb-qemu/.git" ]; then
  git -C "$SRC/asb-qemu" fetch --depth 1 origin "$BRANCH"
  git -C "$SRC/asb-qemu" reset --hard FETCH_HEAD
else
  git clone --depth 1 --branch "$BRANCH" "$FORK" "$SRC/asb-qemu"
fi
cd "$SRC/asb-qemu"

# Minimal configure: raw hypervisor + vmnet NAT + ivshmem only. No GUI/display
# (the VDD is our display over ivshmem), no VNC/SPICE, no TLS/crypto, no host-USB
# passthrough, no qcow2/compression, no slirp (we use vmnet). This drops every
# Homebrew dylib except glib -- see docs. pixman is disabled (no graphics device).
export PKG_CONFIG_PATH="/opt/homebrew/lib/pkgconfig:/opt/homebrew/opt/glib/lib/pkgconfig"
mkdir -p build; cd build
[ -f build.ninja ] || ../configure \
  --target-list=aarch64-softmmu \
  --enable-hvf --enable-vmnet \
  --disable-pixman --disable-slirp --disable-vnc --disable-gtk --disable-sdl \
  --disable-cocoa --disable-curses --disable-spice --disable-opengl --disable-virglrenderer \
  --disable-gnutls --disable-nettle --disable-gcrypt --disable-capstone \
  --disable-libssh --disable-libusb --disable-curl \
  --disable-zstd --disable-bzip2 --disable-lzo --disable-snappy --disable-png \
  --disable-vde --disable-tpm --disable-brlapi --disable-jack --disable-oss \
  --disable-pa --disable-sndio --disable-coreaudio --disable-docs --disable-tools
ninja qemu-system-aarch64
mkdir -p "$OUT/bin"
cp -f qemu-system-aarch64 "$OUT/bin/qemu-system-aarch64-ivshmem"
echo "==> built: $OUT/bin/qemu-system-aarch64-ivshmem"
"$OUT/bin/qemu-system-aarch64-ivshmem" -device ivshmem-plain,help >/dev/null && echo "==> ivshmem-plain present ✓"
echo "==> non-system deps (should be glib only):"
otool -L "$OUT/bin/qemu-system-aarch64-ivshmem" | grep -vE "/System/|/usr/lib/" | grep -E "/opt/|/usr/local/" || true
