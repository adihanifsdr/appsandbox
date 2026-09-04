# VM identity profile (Linux guests)

A per-VM JSON profile that decides what the guest reports about the machine
it runs on. Set it in **New Sandbox → VM identity (JSON)**, with
`vmIdentity` on `POST /vms` / `PUT /vms/{name}` in the headless API, or by
writing `/etc/appsandbox/identity.json` inside the guest. The host hands the
profile to the guest agent every time it connects; the agent stores it and
runs `appsandbox-identity apply`, which also runs at every boot
(`appsandbox-identity.service`).

Profile format (either shape):

```json
[
  {"check": "sys_vendor",          "name": "OpenStack Foundation"},
  {"check": "product_name",        "name": "OpenStack Nova"},
  {"check": "bios_vendor",         "name": "SeaBIOS 1.16.3-debian"},
  {"check": "chassis_vendor",      "name": "QEMU"},
  {"check": "chipset",             "name": "440FX + PIIX3, Cirrus VGA, virtio"},
  {"check": "systemd-detect-virt", "name": "kvm"}
]
```

## Example: a bare-metal desktop profile (the GUI's default)

**New Sandbox** prefills this profile for Linux guests (the `default` link
restores it, `none` clears it; the per-VM 🪪 editor has a Default button).
What a self-built ASUS/AMI desktop really reports (ASUS boards ship the
literal "System Product Name" / "Default string" values). The guest overlay
takes the DMI keys and `systemd-detect-virt`; the replica takes those plus
the ACPI / SMBIOS / drive / USB / CPUID keys once it runs the
identity-patched QEMU. `chipset` is left out on purpose: the only overlay
available is the 440FX/QEMU list, and the replica's PCI devices (virtio,
Cirrus) stay visible either way.

```json
[
  {"check": "sys_vendor",          "name": "ASUS"},
  {"check": "product_name",        "name": "System Product Name"},
  {"check": "product_version",     "name": "System Version"},
  {"check": "product_serial",      "name": "System Serial Number"},
  {"check": "product_family",      "name": "To be filled by O.E.M."},
  {"check": "bios_vendor",         "name": "American Megatrends Inc."},
  {"check": "bios_version",        "name": "2604"},
  {"check": "bios_date",           "name": "01/15/2024"},
  {"check": "board_vendor",        "name": "ASUSTeK COMPUTER INC."},
  {"check": "board_name",          "name": "ROG STRIX Z690-F GAMING WIFI"},
  {"check": "board_version",       "name": "Rev 1.xx"},
  {"check": "board_serial",        "name": "230712345678901"},
  {"check": "chassis_vendor",      "name": "Default string"},
  {"check": "chassis_type",        "name": "3"},
  {"check": "chassis_version",     "name": "Default string"},
  {"check": "chassis_serial",      "name": "Default string"},
  {"check": "systemd-detect-virt", "name": "none"},
  {"check": "acpi_oem_id",         "name": "ALASKA"},
  {"check": "acpi_oem_table_id",   "name": "A M I"},
  {"check": "acpi_creator_id",     "name": "AMI"},
  {"check": "smbios_manufacturer", "name": "Intel(R) Corporation"},
  {"check": "drive_vendor",        "name": "HL-DT-ST"},
  {"check": "cdrom_model",         "name": "DVDRAM GH24NSD1"},
  {"check": "disk_model",          "name": "Samsung SSD 870 EVO 1TB"},
  {"check": "usb_vendor",          "name": "Logitech"},
  {"check": "CPUID 0x40000000",    "name": "GenuineIntel"},
  {"check": "hypervisor CPU flag", "name": "not set"},
  {"check": "smbios_vm_bit",       "name": "not set"}
]
```

`systemd-detect-virt` set to `none` also makes the wrapper exit 1 (and
`-q` exit 1), the way the real tool answers on bare metal, and `chassis_type`
3 makes `hostnamectl` say "Chassis: desktop" instead of "vm". In the replica
the `GenuineIntel` signature matches no hypervisor and `smbios_vm_bit`
clears the SMBIOS "Virtual Machine" flag, so together with the hidden
hypervisor bit the real `systemd-detect-virt` answers `none` even as root
and a guest kernel treats the machine as bare metal (and stops using
kvm-clock).

## What the overlay changes inside the guest

| check | how |
|---|---|
| any `/sys/class/dmi/id` key (`sys_vendor`, `product_name`, `bios_vendor`, `chassis_vendor`, `board_vendor`, `product_serial`, `product_uuid`, ...) | the value is bind-mounted over the sysfs attribute, so every reader of sysfs sees it |
| `dmidecode -s <keyword>` | diverted to a wrapper answering from those keys; other invocations run the real binary |
| `systemd-detect-virt` | diverted to a wrapper printing the profile value (`-q`, `-c`, `--list`, `--help` behave as usual) |
| `chipset` naming 440FX / PIIX3 | `lspci` (no arguments) prints the classic QEMU/OpenStack device list |

## What it cannot change (hypervisor level)

`CPUID 0x40000000` (always `Microsoft Hv` under Hyper-V), the `hypervisor`
CPU flag (already set), the clocksource (`hyperv_clocksource_tsc_page`),
and the real PCI topology. A program that reads CPUID directly still sees
Hyper-V. For a faithful KVM/OpenStack machine — SeaBIOS, i440FX + PIIX3,
Cirrus, virtio, `KVMKVMKVM`, `kvm-clock` — use the nested-KVM replica
instead, which runs a real QEMU/KVM guest inside the sandbox VM.

## Keys the replica honours (same profile)

The profile may also carry what only the emulator decides. `appsandbox-identity`
skips these ("emulator level"); `appsandbox-replica` passes them to its QEMU
as `QEMU_IDENTITY_*` environment variables, honoured once the guest has built
the identity-patched QEMU (`sudo appsandbox-replica qemu build`, see
`tools/linux/replica/qemu-identity/README.md`):

| check | default | meaning |
|---|---|---|
| `acpi_oem_id`, `acpi_oem_table_id`, `acpi_creator_id` | `BOCHS`, `BXPC`, `BXPC` | ACPI table header IDs |
| `smbios_manufacturer` | `QEMU` | manufacturer of the SMBIOS entries `<sysinfo>` leaves alone (processor, DIMMs) |
| `smbios_vm_bit` | `set` | `not set` clears the SMBIOS type 0 "Virtual Machine" flag (implied by `hypervisor CPU flag: not set`) |
| `drive_vendor`, `disk_model`, `cdrom_model` | `QEMU`, `QEMU HARDDISK`, `QEMU DVD-ROM` | IDE / ATAPI / SCSI INQUIRY strings |
| `usb_vendor` | `QEMU` | USB HID / tablet string descriptors |
| `CPUID 0x40000000` | `KVMKVMKVM` | hypervisor CPUID signature (a non-KVM value also drops kvm-clock) |
| `hypervisor CPU flag` | `set` | `not set` hides the CPUID hypervisor bit (stock QEMU can) |

Every profile change the agent receives also redefines an existing replica
(`appsandbox-replica reidentify`); the replica shows it at its next boot.

`appsandbox-identity status` shows what is currently visible and where it
comes from (`firmware` vs `overlay`); `appsandbox-identity remove` undoes
everything.
