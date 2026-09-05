#!/usr/bin/env python3
"""Regenerate the Nestbox app icons from docs/brand/*.svg.

    python tools/brand/make-icons.py

Writes src/app_win/appsandbox.ico, the macOS AppIcon set, and the
docs/brand/nestbox-app-icon-*.png previews.

The SVGs are rendered by headless Edge (or Chrome) rather than a Python SVG
library, so what ships is exactly what the browser draws in the UI. Pillow does
the downsampling and packs the .ico.
"""
import io
import os
import struct
import subprocess
import sys
import tempfile

from PIL import Image

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
BRAND = os.path.join(REPO, "docs", "brand")

CHASSIS = "#191B22"
ACCENT = "#9497F6"
ACCENT_DIM = "#6F72C9"

# Below this the three-shell mark turns to mush, so those sizes are rendered
# from the two-shell drawing instead.
SMALL_BELOW = 96

BROWSERS = [
    r"C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe",
    r"C:\Program Files\Microsoft\Edge\Application\msedge.exe",
    r"C:\Program Files\Google\Chrome\Application\chrome.exe",
    "/usr/bin/microsoft-edge",
    "/usr/bin/google-chrome",
    "/usr/bin/chromium",
]

PAGE = """<!doctype html><meta charset="utf-8">
<style>
  html,body {{ margin:0; background:transparent; }}
  .icon {{ width:{px}px; height:{px}px; position:relative; }}
  .chassis {{
     position:absolute; inset:0; border-radius:{r}px; background:{chassis};
     border:{hair}px solid rgba(148,151,246,.14); box-sizing:border-box;
  }}
  .mark {{ position:absolute; inset:{inset}px; color:{accent}; }}
  svg {{ width:100%; height:100%; display:block; }}
</style>
<div class="icon"><div class="chassis"></div><div class="mark">{svg}</div></div>
"""


def find_browser():
    for path in BROWSERS:
        if os.path.exists(path):
            return path
    sys.exit("no Edge or Chrome found; install one or edit BROWSERS")


def render(browser, tmp, name, px, svg_file, inset_ratio, scale=1):
    """Shoot one icon source page and return it as a Pillow image."""
    svg = open(os.path.join(BRAND, svg_file), encoding="utf-8").read()
    svg = svg.replace('width="24" height="24"', "")
    svg = svg.replace('stroke="currentColor"', 'stroke="%s"' % ACCENT)
    svg = svg.replace('opacity="0.45"', 'opacity="0.45" stroke="%s"' % ACCENT_DIM)
    html = PAGE.format(px=px, r=round(px * 0.225), chassis=CHASSIS, accent=ACCENT,
                       hair=max(1, round(px / 512)), inset=round(px * inset_ratio), svg=svg)
    page = os.path.join(tmp, name + ".html")
    shot = os.path.join(tmp, name + ".png")
    open(page, "w", encoding="utf-8").write(html)
    subprocess.run([
        browser, "--headless=new", "--disable-gpu", "--hide-scrollbars",
        "--default-background-color=00000000",
        "--force-device-scale-factor=%d" % scale,
        "--window-size=%d,%d" % (px, px),
        "--screenshot=" + shot,
        "file:///" + page.replace("\\", "/"),
    ], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    return Image.open(shot).convert("RGBA")


def dib_entry(im):
    """32bpp bottom-up BGRA DIB plus an empty AND mask, as an .ico entry expects."""
    w, h = im.size
    px = im.load()
    xor = bytearray()
    for y in range(h - 1, -1, -1):
        for x in range(w):
            r, g, b, a = px[x, y]
            xor += bytes((b, g, r, a))
    mask_stride = ((w + 31) // 32) * 4
    header = struct.pack("<IiiHHIIiiII", 40, w, h * 2, 1, 32, 0, len(xor), 0, 0, 0, 0)
    return header + bytes(xor) + bytes(mask_stride * h)


def png_entry(im):
    buf = io.BytesIO()
    im.save(buf, format="PNG", optimize=True)
    return buf.getvalue()


def write_ico(path, scaled, sizes):
    blobs = [(px, png_entry(scaled(px)) if px >= 128 else dib_entry(scaled(px)))
             for px in sizes]
    offset = 6 + 16 * len(blobs)
    out = bytearray(struct.pack("<HHH", 0, 1, len(blobs)))
    for px, blob in blobs:
        # 256 is written as 0, which is what the .ico directory means by it.
        out += struct.pack("<BBBBHHII", px % 256, px % 256, 0, 0, 1, 32, len(blob), offset)
        offset += len(blob)
    for _, blob in blobs:
        out += blob
    open(path, "wb").write(out)
    print("ico  %s  %d bytes  %s" % (path, len(out), sizes))


MAC_PLAN = [
    ("icon_16x16.png", 16), ("icon_16x16@2x.png", 32),
    ("icon_32x32.png", 32), ("icon_32x32@2x.png", 64),
    ("icon_128x128.png", 128), ("icon_128x128@2x.png", 256),
    ("icon_256x256.png", 256), ("icon_256x256@2x.png", 512),
    ("icon_512x512.png", 512), ("icon_512x512@2x.png", 1024),
]


def main():
    browser = find_browser()
    with tempfile.TemporaryDirectory() as tmp:
        big = render(browser, tmp, "icon-1024", 1024, "nestbox-mark.svg", 0.20)
        # Rendered at 256 CSS px with a 4x device scale, so the two-shell mark is
        # laid out for a small icon but downsampled from a 1024px shot.
        small = render(browser, tmp, "icon-256", 256, "nestbox-mark-small.svg", 0.19, scale=4)

        def scaled(px):
            src = big if px >= SMALL_BELOW else small
            return src.resize((px, px), Image.LANCZOS)

        write_ico(os.path.join(REPO, "src", "app_win", "appsandbox.ico"),
                  scaled, [16, 32, 48, 64, 128, 256])

        macdir = os.path.join(REPO, "src", "app_mac", "Assets.xcassets", "AppIcon.appiconset")
        for name, px in MAC_PLAN:
            scaled(px).save(os.path.join(macdir, name), format="PNG", optimize=True)
        print("mac  %d files -> %s" % (len(MAC_PLAN), macdir))

        for px in (1024, 512, 256, 128, 64, 32):
            scaled(px).save(os.path.join(BRAND, "nestbox-app-icon-%d.png" % px),
                            format="PNG", optimize=True)
        print("png  previews -> %s" % BRAND)


if __name__ == "__main__":
    main()
