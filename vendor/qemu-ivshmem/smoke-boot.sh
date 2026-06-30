#!/usr/bin/env bash
# Firmware smoke-test for the vendored fork QEMU. Boots just the EDK2 firmware (no disk)
# under HVF and shows its UEFI output on the serial console. Proves the new minimal build
# + the HVF entitlement + the gzipped firmware are a valid VM engine. No root needed (no
# vmnet here); self-stops after ~12s. Run it in a Terminal: bash smoke-boot.sh
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
DIST="$HERE/dist"
QEMU="$DIST/bin/qemu-system-aarch64"
[ -x "$QEMU" ] || { echo "run ./build-qemu.sh && ./stage.sh first"; exit 1; }

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
gunzip -c "$DIST/share/qemu/edk2-aarch64-code.fd.gz" > "$WORK/code.fd"
gunzip -c "$DIST/share/qemu/edk2-arm-vars.fd.gz"     > "$WORK/vars.fd"

echo "== booting EDK2 firmware on the vendored fork QEMU (HVF) — watch for the TianoCore/UEFI banner =="
"$QEMU" -machine virt,gic-version=3,highmem=on -cpu host -accel hvf -m 2048 -smp 2 \
  -drive if=pflash,format=raw,readonly=on,file="$WORK/code.fd" \
  -drive if=pflash,format=raw,file="$WORK/vars.fd" \
  -display none -serial stdio -no-reboot &
QPID=$!
sleep 12
kill "$QPID" 2>/dev/null || true
wait "$QPID" 2>/dev/null || true
echo
echo "== if UEFI/TianoCore output appeared above, the new engine is VALID (HVF + firmware OK) =="
