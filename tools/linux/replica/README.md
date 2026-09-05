# VPS replica (nested KVM)

`appsandbox-replica` runs a real QEMU/KVM guest *inside* an App Sandbox
Linux VM (nested virtualization is on for Linux guests), shaped like a
KVM / OpenStack Nova VPS. Unlike the guest-side identity overlay, nothing is
faked here: it is a KVM guest, so every hypervisor-level check matches too.

| check | replica |
|---|---|
| sys_vendor / product_name / bios_vendor / chassis_vendor | from the VM identity profile via `<sysinfo type='smbios'>` (defaults: OpenStack Foundation / OpenStack Nova / SeaBIOS 1.16.3-debian / QEMU) |
| chipset | i440FX + PIIX3, Cirrus VGA, virtio disk / net / balloon |
| CPUID 0x40000000 | `KVMKVMKVM` (`<cpu mode='host-passthrough'/>`); the profile value with the identity-patched QEMU |
| hypervisor CPU flag | set; `"not set"` in the profile hides it (`<feature policy='disable' name='hypervisor'/>`) |
| systemd-detect-virt | `kvm` |
| clocksource | `kvm-clock` |
| acpi_oem_id / acpi_oem_table_id / acpi_creator_id | `BOCHS` / `BXPC` / `BXPC`; the profile values with the identity-patched QEMU |
| smbios_manufacturer (processor / DIMM manufacturer) | `QEMU`; the profile value with the identity-patched QEMU |
| smbios_vm_bit (SMBIOS "Virtual Machine" flag) | set; `not set` (or a hidden hypervisor flag) clears it with the identity-patched QEMU |
| drive_vendor / disk_model / cdrom_model | `QEMU` / `QEMU HARDDISK` / `QEMU DVD-ROM`; the profile values with the identity-patched QEMU |
| usb_vendor | `QEMU` (usb-tablet & co.); the profile value with the identity-patched QEMU |

The "identity-patched QEMU" is upstream QEMU 8.2.2 plus the patches in
[`qemu-identity/`](qemu-identity/README.md) (the parameterised form of
[kila58/qemu-patched](https://github.com/kila58/qemu-patched)): every string
above reads a `QEMU_IDENTITY_*` environment variable, which the replica's
domain XML sets from the profile (`<qemu:env>`). Built once inside the
sandbox VM with `sudo appsandbox-replica qemu build` and installed over the
distro binary with `dpkg-divert` (`qemu restore` undoes it). Until then the
stock QEMU simply ignores those keys and `create` / `reidentify` say so.

## Several replicas per sandbox

Every replica has a name (`-n NAME`, default `replica`), its own libvirt
domain, disk, ssh key and VNC port, under
`/var/lib/appsandbox/replica/replicas/<name>/`; the base image and the
patched QEMU are shared. `appsandbox-replica list` prints them as JSON, which
the guest agent reports to Nestbox so each replica gets its own row (start,
screen, XFCE desktop, stop, restart, delete) under its sandbox, and the `+`
in the nested column creates another one. `nestbox-replica` is an alias.

## Use

Inside the sandbox VM (SSH in with the `>_` button):

```
sudo appsandbox-replica install                   # qemu + libvirt (once)
sudo appsandbox-replica create                    # downloads the Ubuntu 24.04 cloud image, boots the replica
sudo appsandbox-replica checks                    # prints the checks as [{check,name}] JSON
sudo appsandbox-replica ssh                       # user / test123, key auth
sudo appsandbox-replica qemu build                # once: identity-patched QEMU (ACPI / SMBIOS / drive / CPUID strings)
sudo appsandbox-replica reidentify [--restart]    # re-read the profile into the domain (the agent does this on every change)
sudo appsandbox-replica desktop                   # XFCE + autologin inside the replica, virtio video 1600x900
```

`desktop` makes the replica usable from the App Sandbox **VNC button**: it
installs xubuntu-core with LightDM autologin for `user` and switches the video
model from Cirrus to virtio (`--video` on `create` / `reidentify` does the
same). There is no GPU in the replica, so rendering is llvmpipe: fine for the
desktop and light applications, not for GPU-heavy ones.

Editing the VM identity in the App Sandbox GUI (🪪) or with `vmIdentity` in
the headless API pushes the profile to the guest agent, which re-applies the
guest overlay and, when a replica exists, runs `appsandbox-replica
reidentify`. ACPI, SMBIOS, drive and CPUID strings are fixed at boot, so the
replica shows the new identity after its next `stop` / `start` (or
`reidentify --restart`).

`create` options: `--smbios sysinfo|host` (`host` passes the *sandbox VM's*
SMBIOS through instead of the profile strings - under App Sandbox that is
Hyper-V's "Microsoft Corporation / Virtual Machine"), `--disk 20G`,
`--ram 4096`, `--cpus 4`, `--vnc 5900`, `--profile <json>`, `--image <url>`.
The generated libvirt XML is at `/var/lib/appsandbox/replica/replica.xml`;
edit it and `virsh define` it to tweak anything.

`resize [--cpus N] [--ram MiB] [--disk 30G] [--restart]` changes an existing
replica: cores and RAM are redefined and apply at its next boot (right away
with `--restart`); the disk only grows (live when the replica runs), and the
guest's cloud-init growpart extends the root filesystem at the next boot.
The pencil on a replica row in Nestbox does the same, and the "+" dialog
takes the size of a new replica; `list` reports `cpus`, `ram` and `disk`.

The replica's console is a VNC server on `127.0.0.1:5900` inside the sandbox
VM, which is what the App Sandbox **VNC button** tunnels to - so the button
shows the replica's screen. `ssh` / `checks` reach it over libvirt's NAT
network (`virsh domifaddr replica`).

Sizing: the sandbox VM needs a few GB free (packages ~400 MB, base image
~270 MB, plus the replica disk) and enough RAM for both; a 4-vCPU / 4 GB
replica inside an 8 GB sandbox VM is comfortable.
