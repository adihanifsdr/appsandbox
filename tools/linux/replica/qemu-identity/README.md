# Identity-patched QEMU for the VPS replica

The nested replica (`appsandbox-replica`) is a real KVM guest, so the
hypervisor-level checks match a KVM/OpenStack machine by construction. A few
strings, though, are decided by the emulator and hardcoded in upstream QEMU:
the ACPI OEM / creator IDs (`BOCHS ` / `BXPC`), the SMBIOS default
manufacturer (`QEMU`, visible as the processor and DIMM manufacturer), the
drive vendor and models (`QEMU HARDDISK`, `QEMU DVD-ROM`), the USB vendor
strings and the CPUID `0x40000000` signature (`KVMKVMKVM`).

[kila58/qemu-patched](https://github.com/kila58/qemu-patched) is QEMU 8.1.3
with exactly those strings replaced by `TOSH…` constants. `patches/` is the
same set of changes (plus the SMBIOS "Virtual Machine" bit, which it
missed) in a parameterised form: each string consults a
`QEMU_IDENTITY_<KEY>` environment variable and falls back to the upstream
value, so one build serves every VM identity profile and behaves like stock
QEMU when nothing is set.

| profile key (`check`) | environment variable | upstream default | where the guest sees it |
|---|---|---|---|
| `acpi_oem_id` | `QEMU_IDENTITY_ACPI_OEM_ID` | `BOCHS ` (6) | every ACPI table header, `/sys/firmware/acpi/tables/*` |
| `acpi_oem_table_id` | `QEMU_IDENTITY_ACPI_OEM_TABLE_ID` | `BXPC    ` (8) | same |
| `acpi_creator_id` | `QEMU_IDENTITY_ACPI_CREATOR_ID` | `BXPC` (4) | same |
| `smbios_manufacturer` | `QEMU_IDENTITY_SMBIOS_MANUFACTURER` | `QEMU` | `dmidecode -t 4` / `-t 17`, any SMBIOS entry `<sysinfo>` does not set |
| `drive_vendor` | `QEMU_IDENTITY_DRIVE_VENDOR` | `QEMU` | ATAPI / SCSI INQUIRY vendor, `/sys/block/sr0/device/vendor` |
| `disk_model` | `QEMU_IDENTITY_HD_MODEL` | `QEMU HARDDISK` | IDE / SCSI disks (`lsblk -o MODEL`) |
| `cdrom_model` | `QEMU_IDENTITY_CD_MODEL` | `QEMU DVD-ROM` / `QEMU CD-ROM` | IDE / SCSI CD-ROMs, `/sys/block/sr0/device/model` |
| `usb_vendor` | `QEMU_IDENTITY_USB_VENDOR` | `QEMU` | usb-tablet / mouse / kbd / wacom string descriptors (`lsusb`) |
| `CPUID 0x40000000` | `QEMU_IDENTITY_KVM_SIGNATURE` | `KVMKVMKVM` | the hypervisor CPUID leaf (`cpuid`, `/dev/cpu/*/cpuid`) |
| `smbios_vm_bit` | `QEMU_IDENTITY_SMBIOS_VM_BIT` (`0`) | set | SMBIOS type 0 "Virtual Machine" characteristic: what `systemd-detect-virt` reports as `vm-other` (root) when every string says otherwise |

`hypervisor CPU flag: "not set"` needs no patch: the replica hides the CPUID
hypervisor bit with libvirt's `<feature policy='disable' name='hypervisor'/>`.

A word on the signature: a Linux guest binds kvm-clock and the other KVM
paravirt features only when it finds the canonical `KVMKVMKVM`; with any
other value it falls back to the TSC clocksource, so the `clocksource`
check changes too. That is the intended effect when the key is set.

`appsandbox-replica` maps the profile onto `<qemu:env>` entries in the
domain XML; `appsandbox-replica checks` reads all of them back from inside
the replica.

## Patches

Written against QEMU 8.2.2 (what Ubuntu 24.04 ships); they also apply to
8.1.3 (the qemu-patched base).

| patch | files | what |
|---|---|---|
| 0001 util | `util/identity.c`, `include/qemu/identity.h` | `qemu_identity_str()` / `qemu_identity_fixed()` helpers |
| 0002 acpi | `hw/i386/x86.c`, `hw/acpi/aml-build.c`, `hw/acpi/core.c` | OEM ID, OEM table ID, creator ID (also the `-acpitable` template header) |
| 0003 i386/pc | `hw/i386/pc_piix.c`, `hw/i386/pc_q35.c` | SMBIOS default manufacturer (i440FX and Q35) |
| 0004 ide/scsi | `hw/ide/core.c`, `hw/ide/atapi.c`, `hw/scsi/scsi-disk.c` | drive vendor / disk model / CD-ROM model |
| 0005 usb | `hw/usb/dev-hid.c`, `hw/usb/dev-wacom.c` | HID and Wacom manufacturer / product strings |
| 0006 i386/kvm | `target/i386/kvm/kvm.c` | CPUID 0x40000000 signature |
| 0007 smbios | `hw/smbios/smbios.c` | the type 0 "Virtual Machine" characteristic bit |

Explicit device properties (`model=`, `vendor=`, `product=`, `-smbios`,
`-machine x-oem-id=`) still win over the environment. Not carried over from
qemu-patched: renaming the `bochs` block-driver format (only `qemu-img` ever
sees it) and the monitor-only device descriptions.

## Build and install (inside the sandbox VM)

```
sudo appsandbox-replica qemu build       # ~10 min on 4 vCPUs, ~1.5 GB in /var/lib/appsandbox/replica/qemu-build
sudo appsandbox-replica qemu status
sudo appsandbox-replica reidentify --restart   # replica picks the profile up
sudo appsandbox-replica checks
sudo appsandbox-replica qemu restore     # distro binary back
```

`build.sh` (this directory is shipped in the guest image as
`/opt/appsandbox/qemu-identity/`) installs the build dependencies, downloads
`qemu-8.2.2.tar.xz` from download.qemu.org (sha256-pinned; `--tarball FILE`
uses a copy you already have), applies the patches, configures
`x86_64-softmmu` only (KVM + VNC, no GUI / tools / docs) and builds
`qemu-system-x86_64`.

The binary is installed **over the distro one with `dpkg-divert`**
(`/usr/bin/qemu-system-x86_64`, the distro binary kept at
`…x86_64.distrib`). That path is deliberate: libvirt's AppArmor profile
only lets a domain execute `/usr/bin/qemu-system-*` and libvirtd only probe
binaries under `/usr/bin`; the build is configured with `--prefix=/usr` and
Ubuntu's firmware search path (`/usr/share/qemu`, `/usr/share/seabios`,
`/usr/lib/ipxe/qemu`), so it uses the SeaBIOS 1.16.3 / VGA / iPXE ROMs the
`qemu-system-x86` package already installed - the AppArmor profile allows
exactly those; and libvirt re-probes the emulator's capabilities automatically
(the cache is keyed on the binary's ctime, and it is cleared anyway).
Package upgrades of `qemu-system-x86` respect the diversion. Nothing else
on the system changes; `qemu restore` undoes it.

## Licensing

QEMU is GPL-2.0 (the patched files GPL-2.0-or-later). Nothing prebuilt is
shipped: the guest downloads upstream source and builds it locally, and the
patches here are the complete corresponding source of that build. See
`THIRD-PARTY-NOTICES.md`.
