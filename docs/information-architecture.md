# Information architecture: the Nestbox panel

The structural layer of `web/` (the panel in the Windows app, the same page served
by `tools/linux/host/nestbox` on an Ubuntu host, and the screen viewer). Written
during the 2026-09 redesign; the answers to the usual IA questions are recorded
here with the reasoning, so a later change can be judged against them.

## What a user comes here to do, by frequency

1. See what is running and what is not (every visit).
2. Open a screen: a replica's console, a seat's display, all of them tiled.
3. Start, stop, restart a replica or a seat.
4. Add a seat (often) or a replica (rarely; it takes 10–20 minutes).
5. Create a sandbox (rare; once per machine or per project).
6. Adjust things: a replica's size, a VM's CPU / RAM / GPU / network, the identity
   profile, snapshots (Windows host only).

So the list is the page. There is one view, no navigation between pages, and
everything that takes longer than a click has to say so *in the list*.

## Vocabulary

| Concept | Label in the UI | Notes |
|---|---|---|
| A VM on this PC (Windows host) | **sandbox** | Inherited name; the row at the top level. |
| The machine itself (Linux host) | **this PC** | The single top-level row; it never starts or stops. |
| A KVM guest inside a sandbox / on this PC, with the profile's machine identity | **replica** | "Nested replica" in headings. Sized: cores, RAM, disk, screen. |
| A Linux user with an Xvnc display, XFCE and Steam, on the sandbox / PC itself | **seat** | "Steam seat" in headings. Not sized: it takes what its programs take. |
| A window showing a console or display | **screen** | "Screens" = the grid of every running one. |
| What the guest / replica reports about its hardware | **identity** | The per-VM JSON profile. |
| The in-VM agent | (a dot in Status) | Not a column of its own: it only matters while the VM runs, and only as a gate for SSH, replicas and seats. |

One word each, used everywhere: the dialog, the row, the tooltip, the log line.

## The list

A table, one `<tbody>` per sandbox: the sandbox's row, then a row per replica
and per seat under it, then (while the host works) a placeholder row per thing
being created.

### Sandbox row

`Name · OS · Status · CPU · RAM · Disk · GPU · Network · Snapshot (Windows host) · Actions`

- **Status** carries the lamp, the state word, a spinner while anything is in
  flight, and the agent dot. Every "waiting" state of the VM shows here, never
  in a button.
- **Actions** is one cell with four groups in a fixed order, a slot per button
  even when the button is absent, so the same button sits at the same x on every
  row:
  `[start · display · ssh] [add · screens · identity] [shut down · force stop] [edit · delete]`.
  The old layout had nine icon-only columns whose headers doubled as a legend;
  replica rows reused those columns for different actions (restart under "force
  stop", the grid under "identity"), which is what the redesign removes.

### Replica and seat rows

They are not VMs, so they no longer borrow the VM's columns (a seat had three
empty cells with tooltips explaining why they were empty). One cell spanning
the data columns:

`└ icon  name  KIND  ● state   spec line`

- Spec, replica: `4 cores · 8192 MB · 40 GB · xfce · 800x600 | no desktop · vnc :5900`.
- Spec, seat: `xfce · 1600x900 · steam app 2081880 | steam · vnc :5903`.
- Actions: `[start · screen] [size · desktop] [stop · restart] [delete]`; a seat
  keeps the two empty slots of the second group.
- The **screens** (grid) button belongs to the sandbox row, not to the first
  replica row, because it is the sandbox's set of screens.

### Placeholder rows

A row appears the moment the user asks for something the host will list only
later, and disappears when the host lists it:

| Asked for | Row | Until |
|---|---|---|
| New sandbox | a top-level row `name · OS · Creating…` | the VM is in the list (10 min at most) |
| New replica | under its sandbox, `creating… · cloud image, then XFCE + Steam (10–20 min) · 3:12` | the replica is listed (60 min at most) |
| New seat | under its sandbox, `creating… · packages the first time (~1.5 GB), then seconds · 47s` | the seat is listed (20 min at most) |

A log line that names the thing and says it failed turns the row into
`creating failed · see the log below` for eight seconds. The `×` on the row
stops waiting without cancelling anything.

## The Add dialog

"+" on a sandbox (or on this PC) opens **Add to \<name\>**. The first thing on
it is the choice, as two cards side by side, because the two things are not
variants of one form:

| | Nested replica | Steam seat |
|---|---|---|
| Fields | name, cores, memory, disk, screen size (800 × 600 unless chosen), (build the QEMU patch first — Linux host without it) | user name, screen size; on a Linux host also Steam game, copy from this PC's library, a program to start |
| Ready in | 10–20 min | seconds (packages once) |
| Reserves | cores, RAM, disk | nothing |
| Identity | its own, from the profile | the machine's |

The previous dialog was one form with a "kind" dropdown, the replica's size
fields, and three "Seat:" fields, all shown at once; picking "seat" left cores
/ RAM / disk on screen with a note that they did not apply. That is the question
this document answers: a seat has no resource allocation, so the seat form has
no resource fields.

Names are validated as you type against everything under that sandbox,
including what is still being created. The primary button says which thing it
creates. The last kind chosen and the last seat's game / programs are
remembered in the browser.

Inside a sandbox VM the guest agent takes only the screen size for a seat; the
game and autostart words are honoured on a Linux host only, so those fields are
shown only there and the note says how to set them inside the VM
(`sudo appsandbox-seat -n <name> configure`).

## Waiting states, complete list

Every command has a visible state from the click until the host answers:

| Where | Trigger | Shown | Cleared by |
|---|---|---|---|
| list | page open, before the first state message | three skeleton rows | `fullState` |
| top of page (Linux host) | WebSocket closed | banner "Lost the connection to nestbox. Reconnecting…" | socket open |
| sandbox Status | start / shut down / force stop / delete / save identity | `Starting…` + spinner, buttons off | `running` flips, VM gone, `hasIdentity` matches; 1–3 min at most |
| replica / seat state | start / stop / restart / resize / install desktop / delete | `Restarting…` + amber lamp + spinner, buttons off | the state it waited for, or the host's `…:ok.` / `done.` log line; 1–40 min at most |
| new rows | create sandbox / replica / seat | placeholder row with an elapsed counter | listed by the host |
| identity button | click | spinner in the button | the `identity` message (10 s at most) |
| snapshot cell | take / delete / rename / delete branch | `Taking the snapshot…` + spinner | the snapshot tree changes (2 min at most) |
| CPU / RAM / GPU / network cell | inline edit | spinner after the value | the host echoes the value (8 s) |
| template list | delete | `deleting…` on the item | the template is gone (1 min) |
| viewer | connecting / lost / stopping / restarting | overlay over the console: spinner or lamp, a sentence, Reconnect or Close | connect event |

Timeouts exist only so a wait can never become permanent; the row then simply
shows what the host says. Nothing here is a claim about the host's state, only
about what the page has asked for.

## Empty states

- No sandboxes: the Nestbox mark, "No sandboxes yet", one sentence on what a
  sandbox can hold, the New sandbox button.
- This PC with nothing under it (Linux host): a hint row, "Nothing here yet. The
  + adds a nested replica or a Steam seat to this PC."

## What stays as it was

The "signal rack" look: warm monochrome, mono labels, lamps for state, one accent
for selection and the primary action, light and dark themes. The log below the
list, collapsible. The Create sandbox dialog (one form; its fields are all real
for a VM). The viewer's bar. All message names between the page and the host.
