# Nestbox on a Linux host

On Ubuntu there is no sandbox VM in between: **this PC is the top row of the
table and the replicas run on it directly** as libvirt / KVM guests, built by
the same `appsandbox-replica` script the sandbox uses (see
[../replica/](../replica/)).

```
<this PC>            Ubuntu 24.04   Running   16 cores  32 GB
  └ replica          running   4 cpu  4096 MB  20 GB   vnc :5900   xfce + steam
  └ replica2         stopped   2 cpu  2048 MB  20 GB
```

`nestbox` here is a small Python program (standard library only) that serves
the same web UI the Windows and macOS apps embed ([../../../web/](../../../web/))
to your browser on `127.0.0.1:8765`, speaks the same JSON messages with it
over a WebSocket, runs `appsandbox-replica` for every button, and bridges the
replica consoles to the viewer page over WebSockets, so screens open in
browser windows (one per replica, or the grid).

## Run

```bash
git clone https://github.com/adihanifsdr/nestbox.git
cd nestbox
sudo tools/linux/host/nestbox            # first run installs qemu / libvirt, then opens the browser
```

Options: `--port 8765`, `--no-browser` (prints the URL), `--qemu-patch`
(build the identity-patched QEMU before creating replicas, ~10 min once),
`--web DIR`.

Root is needed for libvirt and `/dev/kvm`; the browser and an external VNC
viewer are started as the user who ran `sudo`. Check `ls -l /dev/kvm` first:
without it (virtualization disabled in the firmware) replicas cannot run.

## As a service, and from another machine

```bash
sudo git clone https://github.com/adihanifsdr/nestbox.git /opt/nestbox
sudo cp /opt/nestbox/tools/linux/host/nestbox.service /etc/systemd/system/
sudo systemctl daemon-reload && sudo systemctl enable --now nestbox
sudo journalctl -u nestbox -f          # the same lines the UI's log panel shows
```

The UI listens on 127.0.0.1 only; the consoles ride on the same port
(`/vnc/<port>` WebSockets), so a single SSH tunnel is all a remote machine
needs:

```
ssh -N -L 8765:127.0.0.1:8765 root@<server>      # then open http://127.0.0.1:8765
```

Screens and the grid open as browser windows through that tunnel; nothing
is exposed on the server's public address.

## What works

- `+` on the PC row: name and size a new replica; the log shows the steps
  (packages, cloud image download, first boot, XFCE + Steam).
- Replica rows: start, screen (browser window), pencil (cores / RAM / disk),
  desktop, shut down, restart, delete, and the grid button for every running
  replica in one window.
- Identity (the id card on the PC row): the profile in
  `/etc/appsandbox/identity.json`, applied to every replica with
  `reidentify --all` (takes effect at each replica's next boot).
- External viewer: `vncviewer`, Remmina or `xdg-open vnc://` on
  `127.0.0.1:<port>`.

## The QEMU patch

By default the replicas use the distro's QEMU. Everything at the SMBIOS /
DMI level (vendor, board, serial numbers), the hidden hypervisor CPU flag and
the i440FX / SeaBIOS look are still there; only the hypervisor-level strings
(ACPI OEM ids, SMBIOS manufacturer, drive / CD-ROM models, USB vendor, the
CPUID 0x40000000 signature) are ignored.

The PC row shows which QEMU is in place: `qemu: stock` with a **Build patch**
button, or `qemu: identity-patched ✔`. The button builds QEMU 8.2.2 with the
identity patches on this PC and installs it over the distro binary
(dpkg-divert, ~10 minutes, once; no replica needed first). The `+` dialog
has the same as a checkbox for the first replica, and `--qemu-patch` on the
command line does it before any replica is created. Running replicas pick
the patched QEMU up at their next boot.

## Not on Linux

Sandbox VMs (the Hyper-V / Virtualization-framework layer), GPU-PV, the IDD
display, SSH key deploy and snapshots are host features of the Windows and
macOS apps; the Linux host has none of them, and the UI hides them.
