# QEMU + ivshmem (macOS/HVF)

The patched QEMU that gives a Windows guest a shared-memory BAR (ivshmem-plain) on macOS/HVF,
vendored and embedded into AppSandbox.app.

## Source of truth
The QEMU source is our fork **github.com/jamesstringer90/asb-qemu**, branch
**`asb-ivshmem-11.0.1`** = upstream tag `v11.0.1` + 3 ivshmem commits (Kconfig LINUX gate, meson
eventfd gate, `event_notifier_init_fd` unconditional). The fork is our GPL corresponding-source;
there are no local patch files (the changes live as commits on the branch).

## Pipeline
```
RARE, manual (needs brew/meson/ninja):
  ./build-qemu.sh    # clone the fork @ asb-ivshmem-11.0.1, minimal raw-hypervisor configure, ninja
                     #   -> <asb-win-on-mac>/runtime/qemu/bin/qemu-system-aarch64-ivshmem
  ./stage.sh         # gather binary + dylib closure into dist/, rewrite install-names to
                     #   @loader_path/../lib, codesign with qemu.entitlements (HVF!), gzip firmware
                     #   -> dist/  (committed)

EVERY Xcode build (the only script Xcode runs):
  embed-into-bundle.sh   # "Embed QEMU" build phase: copies dist/ -> AppSandbox.app/.../Resources/qemu
```

## Files (committed)
- `build-qemu.sh` — build QEMU from the fork (rare; needs the toolchain).
- `stage.sh` — stage `dist/` from the build output (rare; runs after build-qemu.sh).
- `qemu.entitlements` — `com.apple.security.hypervisor`; stage.sh signs the binary with it, or HVF
  is denied and no VM boots. Survives gzip → embed → app codesign.
- `embed-into-bundle.sh` — the Xcode "Embed QEMU" phase calls this on every build.
- `smoke-boot.sh` — boot just the EDK2 firmware under HVF to validate the engine: `bash smoke-boot.sh`.
- `dist/` — committed, self-contained: `bin/qemu-system-aarch64`, `lib/*.dylib` (glib×4 + libintl +
  libpcre2), `share/qemu/edk2-{code,vars}.fd.gz`. ~40M. (Firmware gzipped 128M → 1.6M.)

## Runtime resolution (qemu_vm.m)
Bundle first (`Contents/Resources/qemu/...`), dev-fallback = walk up to `dist/`. Firmware is
gunzipped at VM-create with the base-system `/usr/bin/gunzip` (zero dev-tool deps): code.fd → a
shared `<appsupport>/firmware/` cache, vars.fd → per-VM. NIC uses `romfile=` (efi-virtio.rom dropped).
