# The Nestbox mark

An outer hexagonal shell, a second shell nested inside it, and at the centre the box
itself drawn as an isometric cube: the sandbox, the nested hypervisor inside it, and
the replica inside that. One accent, stroked on a 24-unit grid like the rest of the
interface glyphs.

| File | Use |
|---|---|
| `nestbox-mark.svg` | Three shells. The full drawing, for 32px and up. |
| `nestbox-mark-small.svg` | Two shells; the middle one is dropped so the cube survives 16-20px. This is the form inlined in `web/index.html` as `#i-nestbox`. |
| `nestbox-mark-512-lavender.png` | Transparent mark, `#9497F6` — the dark-theme accent. |
| `nestbox-mark-512-indigo.png` | Transparent mark, `#4437D4` — the light-theme accent. |
| `nestbox-app-icon-*.png` | The app icon: the mark in lavender on a `#191B22` chassis, rounded at 22.5%. |

Both SVGs stroke in `currentColor`, so inline they take whatever `--accent` the theme
is on.

The app icons are generated from the SVGs, not drawn separately, and they feed:

- `src/app_win/appsandbox.ico` — 16/32/48/64 as 32-bit DIBs, 128/256 as PNG.
- `src/app_mac/Assets.xcassets/AppIcon.appiconset/` — the ten sizes Xcode asks for.

Sizes below 96px are rendered from the two-shell drawing; above it, from the three-shell
one. Edit an SVG and run `python tools/brand/make-icons.py` to rebuild all of them; it
shoots the SVGs with headless Edge (or Chrome) so what ships is what the browser draws,
and needs only Pillow.

The first concepts came from `google/nano-banana` on Replicate; the mark shipped here was
redrawn by hand from that direction so the geometry is exact.
