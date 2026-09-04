# VPS replica (nested KVM)

`appsandbox-replica` runs a real QEMU/KVM guest *inside* an App Sandbox
Linux VM (nested virtualization is on for Linux guests), shaped like a
KVM / OpenStack Nova VPS. Unlike the guest-side identity overlay, nothing is
faked here: it is a KVM guest, so every hypervisor-level check matches too.

| check | replica |
|---|---|
| sys_vendor / product_name / bios_vendor / chassis_vendor | from the VM identity profile via `<sysinfo type='smbios'>` (defaults: OpenStack Foundation / OpenStack Nova / SeaBIOS 1.16.3-debian / QEMU) |
| chipset | i440FX + PIIX3, Cirrus VGA, virtio disk / net / balloon |
| CPUID 0x40000000 | `KVMKVMKVM` (`<cpu mode='host-passthrough'/>`) |
| hypervisor CPU flag | set |
| systemd-detect-virt | `kvm` |
| clocksource | `kvm-clock` |

## Use

Inside the sandbox VM (SSH in with the `>_` button):

```
sudo appsandbox-replica install                   # qemu + libvirt (once)
sudo appsandbox-replica create                    # downloads the Ubuntu 24.04 cloud image, boots the replica
sudo appsandbox-replica checks                    # prints the nine checks as [{check,name}] JSON
sudo appsandbox-replica ssh                       # user / test123, key auth
```

`create` options: `--smbios sysinfo|host` (`host` passes the *sandbox VM's*
SMBIOS through instead of the profile strings - under App Sandbox that is
Hyper-V's "Microsoft Corporation / Virtual Machine"), `--disk 20G`,
`--ram 4096`, `--cpus 4`, `--vnc 5900`, `--profile <json>`, `--image <url>`.
The generated libvirt XML is at `/var/lib/appsandbox/replica/replica.xml`;
edit it and `virsh define` it to tweak anything.

The replica's console is a VNC server on `127.0.0.1:5900` inside the sandbox
VM, which is what the App Sandbox **VNC button** tunnels to - so the button
shows the replica's screen. `ssh` / `checks` reach it over libvirt's NAT
network (`virsh domifaddr replica`).

Sizing: the sandbox VM needs a few GB free (packages ~400 MB, base image
~270 MB, plus the replica disk) and enough RAM for both; a 4-vCPU / 4 GB
replica inside an 8 GB sandbox VM is comfortable.
