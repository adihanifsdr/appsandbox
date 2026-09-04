#!/bin/bash
# build.sh - build the identity-patched QEMU for the App Sandbox VPS replica
#
# Downloads the upstream QEMU tarball (sha256-pinned, the same release Ubuntu
# 24.04 ships), applies patches/*.patch (the QEMU_IDENTITY_* environment
# overrides, see README.md), builds only qemu-system-x86_64 with KVM + VNC and
# installs it over the distro binary with dpkg-divert, so libvirt's AppArmor
# profile, the firmware in /usr/share/qemu and the libvirt capabilities cache
# all keep working unchanged. `appsandbox-replica qemu restore` undoes it.
#
# Usage:  build.sh [--tarball FILE] [--work DIR] [--jobs N] [--no-install]
#
#   --tarball FILE  use an already downloaded qemu-<ver>.tar.xz (still verified)
#   --work DIR      build directory (default /var/lib/appsandbox/replica/qemu-build)
#   --jobs N        parallel compile jobs (default: nproc)
#   --no-install    build only; the binary is left in <work>/qemu-<ver>/build
set -euo pipefail

QEMU_VER=8.2.2
QEMU_SHA256=847346c1b82c1a54b2c38f6edbd85549edeb17430b7d4d3da12620e2962bc4f3
QEMU_URL=https://download.qemu.org/qemu-$QEMU_VER.tar.xz
BIN=/usr/bin/qemu-system-x86_64
DIVERT=/usr/bin/qemu-system-x86_64.distrib
STAMP=/opt/appsandbox/qemu-identity.installed

HERE=$(cd "$(dirname "$0")" && pwd)
PATCHES=$HERE/patches
WORK=/var/lib/appsandbox/replica/qemu-build
TARBALL=""
JOBS=$(nproc 2>/dev/null || echo 2)
INSTALL=1

die() { echo "qemu-identity/build.sh: $*" >&2; exit 1; }

while [ $# -gt 0 ]; do
    case "$1" in
        --tarball) TARBALL=$2; shift 2 ;;
        --work) WORK=$2; shift 2 ;;
        --jobs) JOBS=$2; shift 2 ;;
        --no-install) INSTALL=0; shift ;;
        -h|--help) sed -n 2,20p "$0"; exit 0 ;;
        *) die "unknown option $1" ;;
    esac
done
[ "$(id -u)" = 0 ] || die "run as root (sudo)"
[ -d "$PATCHES" ] || die "no patches directory at $PATCHES"

echo "==> build dependencies"
export DEBIAN_FRONTEND=noninteractive
apt-get install -y -q build-essential ninja-build meson python3 python3-venv pkg-config \
    libglib2.0-dev libpixman-1-dev libfdt-dev zlib1g-dev patch xz-utils curl >/dev/null

mkdir -p "$WORK"; cd "$WORK"
if [ -z "$TARBALL" ]; then
    TARBALL=$WORK/qemu-$QEMU_VER.tar.xz
    if ! echo "$QEMU_SHA256  $TARBALL" | sha256sum -c --quiet - 2>/dev/null; then
        echo "==> downloading $QEMU_URL"
        curl -fL --retry 3 -o "$TARBALL.part" "$QEMU_URL" && mv "$TARBALL.part" "$TARBALL"
    fi
fi
echo "==> verifying $TARBALL"
echo "$QEMU_SHA256  $TARBALL" | sha256sum -c --quiet - || die "sha256 mismatch for $TARBALL"

SRC=$WORK/qemu-$QEMU_VER
if [ ! -f "$SRC/.identity-patched" ]; then
    echo "==> extracting"
    rm -rf "$SRC"
    tar -C "$WORK" -xf "$TARBALL"
    echo "==> applying patches"
    for p in "$PATCHES"/*.patch; do
        echo "    $(basename "$p")"
        patch -d "$SRC" -p1 -s -N < "$p"
    done
    touch "$SRC/.identity-patched"
fi

echo "==> configuring (x86_64-softmmu, KVM, VNC)"
mkdir -p "$SRC/build"; cd "$SRC/build"
if [ ! -f build.ninja ]; then
    ../configure --prefix=/usr --target-list=x86_64-softmmu \
        --firmwarepath=/usr/share/qemu:/usr/share/seabios:/usr/lib/ipxe/qemu \
        --enable-kvm --enable-vnc --disable-download \
        --disable-docs --disable-tools --disable-guest-agent --disable-user \
        --disable-gtk --disable-sdl --disable-spice --disable-opengl \
        --disable-virglrenderer --disable-werror >configure.log 2>&1 \
        || { tail -30 configure.log; die "configure failed (see $SRC/build/configure.log)"; }
fi
echo "==> building with $JOBS jobs (a few minutes)"
ninja -j "$JOBS" qemu-system-x86_64 >build.log 2>&1 \
    || { tail -30 build.log; die "build failed (see $SRC/build/build.log)"; }
./qemu-system-x86_64 --version | head -1
# the helper must be linked in: an override has to change what -device reports
QEMU_IDENTITY_CD_MODEL="IDENTITY OK" ./qemu-system-x86_64 -M none -device ide-cd,help >/dev/null 2>&1 || true

if [ "$INSTALL" = 0 ]; then
    echo "==> built (not installed): $SRC/build/qemu-system-x86_64"
    exit 0
fi

echo "==> installing over $BIN (dpkg-divert)"
[ -x "$BIN" ] || [ -x "$DIVERT" ] || die "$BIN is not installed: run 'appsandbox-replica install' first"
if ! dpkg-divert --list "$BIN" 2>/dev/null | grep -q "$DIVERT"; then
    dpkg-divert --quiet --local --rename --divert "$DIVERT" --add "$BIN"
fi
install -m 0755 qemu-system-x86_64 "$BIN"
mkdir -p /opt/appsandbox
printf 'version=%s\nbuilt=%s\n' "$QEMU_VER" "$(date -u +%FT%TZ)" > "$STAMP"
# libvirt caches emulator capabilities keyed on the binary's ctime; make sure
# it re-probes the new one.
rm -f /var/cache/libvirt/qemu/capabilities/*.xml 2>/dev/null || true
systemctl try-restart libvirtd 2>/dev/null || true
echo "==> installed: $BIN is the identity-patched QEMU $QEMU_VER (distro binary kept at $DIVERT)"
echo "    'appsandbox-replica reidentify' (or re-create) passes the profile to the replica;"
echo "    'appsandbox-replica qemu restore' puts the distro binary back."
