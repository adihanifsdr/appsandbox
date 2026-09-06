# Steam seats (several desktops, no VM)

`appsandbox-seat` runs several Steam desktops on one Linux machine without a
virtual machine. A **seat** is a Linux user of its own with an Xvnc display
(TigerVNC), an XFCE session and Steam started at login, run as the systemd
unit `appsandbox-seat@<name>`. Every seat has its own VNC port on
`127.0.0.1`, which is exactly what Nestbox tunnels to: the seat's screen opens
in its own Nestbox window, like a replica's console. Steam keeps its lock in
`$HOME`, so one Steam per user runs side by side with the others.

It works in both places Nestbox knows: inside a sandbox VM (the guest agent
runs the tool; the seat shows as a row under the sandbox) and directly on an
Ubuntu host with `tools/linux/host/nestbox` (the row sits under "this PC",
no `/dev/kvm` needed).

## Seat or replica?

| | Replica (`appsandbox-replica`, nested KVM guest) | Seat (`appsandbox-seat`, a user + Xvnc) |
|---|---|---|
| RAM | reserved per replica (default 4 GB) | only what its programs take |
| CPU | a second layer of virtualization, llvmpipe | the machine's cores directly, llvmpipe on the Xvnc display |
| Ready in | 10-20 min (cloud image, XFCE + Steam per replica) | seconds (packages once, ~1.5 GB) |
| Machine identity | its own: DMI, disks, ACPI, CPUID from the profile | **the machine's**: DMI, machine-id, disk serials, MAC, and inside a sandbox VM the hypervisor flag |
| Isolation | a VM | a user account |

Use seats when you need several Steam clients running at once cheaply. Use
replicas when every instance has to look like a different PC.

## Use

```
sudo appsandbox-seat install [--no-steam]              # packages once: Xvnc, XFCE, Steam
sudo appsandbox-seat -n steam2 create                  # user steam2 (password test123), display, unit; starts it
sudo appsandbox-seat -n steam2 create --resolution 1280x720 --vnc 5905
sudo appsandbox-seat -n steam2 create --steam-app 2081880 \
     --copy-app-from /home/me/snap/steam/common/.local/share/Steam \
     --autostart 'Auto Kathana=/usr/local/bin/auto-kathana'   # Steam opens that game at login, its files
                                                              # copied from that library, the bot started too
sudo appsandbox-seat -n steam2 configure --steam-app 2081880 --autostart 'Auto Kathana=/usr/local/bin/auto-kathana'
                                                       # the same on an existing seat (restarts it)
sudo appsandbox-seat list                              # [{name,state,vnc,desktop,kind:"seat",res}, ...]
sudo appsandbox-seat -n steam2 start|stop|restart|status
sudo appsandbox-seat -n steam2 destroy                 # unit, user and /home/steam2
```

Names are Linux user names: lowercase letters, digits, `-` and `_`, a letter
first (default `seat`). `create` picks the first free VNC port from 5901 up,
skipping the ports of every replica and seat on the machine (`--vnc` asks for
one). Files: `/var/lib/appsandbox/seat/seats/<name>/seat.conf`; the log of a
seat is `journalctl -u appsandbox-seat@<name>`, of its creation
`/var/log/appsandbox-seat-<name>.log` when Nestbox created it.

`--steam-app` makes the seat's Steam start with `steam://rungameid/<id>`, so
the game opens as soon as the seat has signed in to Steam (each seat signs in
on its own; the login is the only thing left to do by hand). `--copy-app-from`
takes the game's `appmanifest_<id>.acf` and `steamapps/common/<dir>` from
another Steam folder on this machine (the snap keeps it under
`~/snap/steam/common/.local/share/Steam`, apt Steam under
`~/.local/share/Steam`) into the seat's library, so it is installed the
moment Steam starts. `--autostart Name=command` adds a program to the seat's
login (repeatable); a program every seat can run has to live outside the
creating user's home, e.g. `/usr/local/bin`. `configure` changes all of this
on an existing seat. From Nestbox, the fields of the "+" dialog do the same.

## The GPU inside a seat

A seat's display is an Xvnc with **DRI3** whenever the machine has a DRM
render node (`/dev/dri/renderD*`): Vulkan, EGL and GLX inside the seat then
run on the real GPU, exactly as on the machine's own desktop. Games need
this to start at all: a bgfx/SDL3 client asks for Vulkan and falls back to
EGL, the Steam client itself insists on a GLX visual, and an X server
without DRI3 gives none of them a device. Ubuntu 24.04's TigerVNC 1.13 has
no DRI3, so `install` (and `create`) unpack Ubuntu 25.04's
`tigervnc-standalone-server` 1.15 into `/opt/tigervnc` (never `dpkg -i`;
upstream's generic build was tried first and lacks GLX) and `run` starts
that `Xtigervnc -rendernode /dev/dri/renderD128`. Each seat gets an X cookie
of its own in `~/.Xauthority`. `status` shows the `gpu:` line. Without a
render node the distro's Xvnc stays and the seat falls back to software GL.

Two things Xvnc lacks that a desktop has, and what `run`/`create` do about
them. There is no real vblank: a Vulkan game in the default FIFO present
mode waits for Xvnc's fake present timer after every frame and crawls at
1-2 fps with the GPU idle (Kathana: 2 fps), so `run` sets
`MESA_VK_WSI_PRESENT_MODE=immediate` for the session (138 fps on the same
seat). Without FIFO nothing paces the game, so when MangoHud is installed
(`apt install mangohud`) `run` also sets `MANGOHUD=1` with
`no_display,fps_limit=30` — the overlay stays hidden, the cap applies to
every Vulkan program in the seat, Steam's runtime imports the host layer.
`SEAT_FPS_LIMIT` in the unit's environment changes the number. And there
is nothing to composite for, so `create` writes an xfwm4 config with
compositing off; on an existing seat:
`xfconf-query -c xfwm4 -p /general/use_compositing -s false`.

Snap apps (Steam) need `/var/lib/snapd/desktop` in `XDG_DATA_DIRS` to show
up in the XFCE menu and Application Finder; `run` adds it, since the
systemd unit starts without a login environment.

## How a seat runs

`/etc/systemd/system/appsandbox-seat@.service` runs `appsandbox-seat -n <name>
run` as the seat's user with `PAMName=login`, so logind gives it a session
(`XDG_RUNTIME_DIR`, a session bus) like a console login. `run` starts
`Xvnc :<n>` on `127.0.0.1:<port>` (no VNC password: the port is loopback-only
and Nestbox reaches it through its tunnel), waits for the socket and runs
`startxfce4` on it; XFCE's autostart then launches Steam. Stopping the unit
ends the session and the display together. Display numbers start at `:11`
(`5901` → `:11`) so they never collide with the machine's own X server.

Easy Anti-Cheat games (Kathana) share one launch lock across every user of
the Steam snap: `/tmp/EasyAntiCheatLauncherSemaphore` inside the snap's
private `/tmp`. The bootstrapper creates it `0700`, so after one user has
played, every other user's launch fails at once with "Failed at essential
procedures, please run the system repair." `start` and `restart` replace it
with a root-owned `0666` file that all users can lock; if the machine's own
user hits the error, `sudo appsandbox-seat -n <any seat> restart` (or just
`sudo rm` the file) clears it.

Rendering is software: the run script sets `LIBGL_ALWAYS_SOFTWARE=1` and
clears the sandbox's d3d12 (GPU-PV) selection, because an Xvnc display has no
DRI. That is fine for the Steam client, the store and light games, the same
as in a replica.

## In Nestbox

The `+` in the nested column asks what to add: a nested replica or a Steam
seat. A seat row has start, screen, stop, restart and delete; the pencil
(size) and the desktop installer only apply to replicas. The grid button
tiles every running screen, replicas and seats alike. Delete removes the user
account and its home directory, Steam's files included.

Inside a sandbox VM the agent runs the tool (`seat <name> <sub>` on the
agent protocol, `seat_result:` back) and reports seats in the same list as
the replicas with `"kind": "seat"`. `nestbox-seat` is an alias.
