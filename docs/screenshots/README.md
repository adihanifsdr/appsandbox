# Screenshots

| File | Shows |
|---|---|
| `nestbox-main.png` | The sandbox list, dark theme: four sandboxes, nested replicas as rows under theirs, the log. |
| `nestbox-light.png` | The same list in the light theme, log collapsed. |
| `nestbox-new-sandbox.png` | New Sandbox on a Linux guest: the nested-replica option and the identity profile. |
| `nestbox-grid.png` | The viewer in grid mode — every running replica of a sandbox tiled in one window. |

All four are rendered by `python tools/brand/make-screenshots.py`, which loads the real
pages from `web/` in headless Edge and feeds them the `fullState` message the native host
would send. The interface in the images is therefore the interface that ships — the CSS,
the markup and `app.js` are the real ones, so a screenshot cannot drift from the UI the
way a hand-taken capture does. Re-run it after a change to the UI.

What is *not* real is the state: the sandbox names, sizes and log lines are sample data,
and the grid's tiles are placeholder desktops rather than live VNC framebuffers, because
a screenshot run has no guest to connect to. They stand in for a console so the tiling is
legible; nothing in them is a claim about what a guest reports.
