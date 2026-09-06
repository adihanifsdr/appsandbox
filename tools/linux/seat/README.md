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

## How a seat runs

`/etc/systemd/system/appsandbox-seat@.service` runs `appsandbox-seat -n <name>
run` as the seat's user with `PAMName=login`, so logind gives it a session
(`XDG_RUNTIME_DIR`, a session bus) like a console login. `run` starts
`Xvnc :<n>` on `127.0.0.1:<port>` (no VNC password: the port is loopback-only
and Nestbox reaches it through its tunnel), waits for the socket and runs
`startxfce4` on it; XFCE's autostart then launches Steam. Stopping the unit
ends the session and the display together. Display numbers start at `:11`
(`5901` → `:11`) so they never collide with the machine's own X server.

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
