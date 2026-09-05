#!/usr/bin/env python3
"""Regenerate docs/screenshots/ from the real web UI.

    python tools/brand/make-screenshots.py

The pages in web/ are the whole interface; the only thing they need from the
native host is a `fullState` message. So this copies web/ to a scratch
directory, drops a small script next to it that feeds the page a sample state,
and shoots the result with headless Edge (or Chrome). Nothing here reimplements
the UI -- the CSS, the markup and app.js are the ones that ship, so a screenshot
cannot drift from the interface the way a hand-cropped capture does.

The state is sample data, not a recording of a real machine: made-up sandbox
names and sizes, and the viewer's tiles are placeholder desktops rather than
live VNC framebuffers. Everything that is Nestbox's own interface -- the table,
the chrome, the modal, the grid -- is genuinely rendered.
"""
import os
import shutil
import subprocess
import sys
import tempfile

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
WEB = os.path.join(REPO, "web")
OUT = os.path.join(REPO, "docs", "screenshots")

WIDTH, HEIGHT = 1400, 820
VIEWER_W, VIEWER_H = 1400, 900

BROWSERS = [
    r"C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe",
    r"C:\Program Files\Microsoft\Edge\Application\msedge.exe",
    r"C:\Program Files\Google\Chrome\Application\chrome.exe",
    "/usr/bin/microsoft-edge",
    "/usr/bin/google-chrome",
    "/usr/bin/chromium",
]

# --------------------------------------------------------------- sample state

DEMO_JS = r"""
/* Sample state for the documentation screenshots. Loaded after app.js, it
   hands the page the same `fullState` message the native host sends, then
   poses whichever shot the query string asks for. */
(function() {
    var shot = (location.search.match(/[?&]shot=([^&]*)/) || [])[1] || 'main';

    var hostInfo = { hostCores: 16, vmCores: 18, hostRamMb: 32611, vmRamMb: 28598,
                     freeGb: 902, vmHddGb: 156 };

    var vms = [
        { name: 'ubuntu-srv', osType: 'Linux', running: false, agentOnline: false,
          installComplete: true, cpuCores: 4, ramMb: 4096, hddGb: 20,
          gpuMode: 1, networkMode: 1, sshEnabled: true, sshState: 0, sshPort: 0,
          hasIdentity: true, replicas: '', snapshots: [], baseBranches: [] },

        { name: 'myappsandbox', osType: 'Linux', running: true, agentOnline: true,
          installComplete: true, cpuCores: 8, ramMb: 16310, hddGb: 32,
          gpuMode: 1, networkMode: 1, sshEnabled: true, sshState: 4, sshPort: 35413,
          hasIdentity: true, snapshots: [], baseBranches: [],
          replicas: JSON.stringify([
              { name: 'replica', state: 'running', vnc: 5900, desktop: true,
                cpus: 4, ram: 8192, disk: 40 },
              { name: 'steam-2', state: 'running', vnc: 5901, desktop: true,
                cpus: 2, ram: 4096, disk: 32 },
              { name: 'build-3', state: 'stopped', vnc: 5902, desktop: false,
                cpus: 2, ram: 2048, disk: 20 }
          ]) },

        { name: 'win11-test', osType: 'Windows', running: true, agentOnline: true,
          installComplete: false, cpuCores: 6, ramMb: 8192, hddGb: 64,
          gpuMode: 2, networkMode: 2, sshEnabled: false, sshState: 0,
          hasSnapshots: true, snapCurrent: -1, snapCurrentBranch: -1,
          snapshots: [{ name: 'base', branches: [] }], baseBranches: [],
          replicas: '' },

        { name: 'kathana-lab', osType: 'Linux', running: true, agentOnline: true,
          installComplete: true, cpuCores: 4, ramMb: 8192, hddGb: 40,
          gpuMode: 0, networkMode: 3, sshEnabled: true, sshState: 2, sshPort: 35414,
          hasIdentity: true, snapshots: [], baseBranches: [],
          replicas: JSON.stringify([
              { name: 'replica', state: 'stopped', vnc: 5900, desktop: false,
                cpus: 4, ram: 4096, disk: 30 }
          ]) }
    ];

    var log = [
        'HCS callback registered [V2] for "myappsandbox".',
        'Starting VM "myappsandbox"...',
        'GPU-PV applied successfully.',
        'Agent online for "myappsandbox".',
        '[myappsandbox] VM identity: applied: sys_vendor, product_name, bios_vendor, board_name, chassis_type, systemd-detect-virt, dmidecode',
        'SSH proxy listening on 127.0.0.1:35413 for "myappsandbox".',
        '[myappsandbox] VNC server listening on guest port 5900.',
        '[myappsandbox] Nested replica "replica": running.',
        '[myappsandbox] Nested replica "steam-2": running.'
    ];

    function pose() {
        if (shot === 'light') document.documentElement.setAttribute('data-theme', 'light');

        window.onHostMessage({ type: 'fullState', vms: vms, hostInfo: hostInfo,
                               adapters: ['Ethernet'], defaultAdapter: 0, templates: [] });
        log.forEach(function(line) { window.onHostMessage({ type: 'log', message: line }); });

        var logSection = document.getElementById('log-section');
        if (logSection) logSection.classList.toggle('collapsed', shot === 'light');

        if (shot === 'new-sandbox') {
            openCreateModal();
            /* Linux, so the fields that make Nestbox what it is are on screen:
               the nested-replica option and the identity profile. */
            document.getElementById('os-type').value = 'Linux';
            applyOsTypeUI();
            document.getElementById('replica-auto').checked = true;
            /* A name that is already taken, so the inline validation shows. */
            var nameEl = document.getElementById('vm-name');
            nameEl.value = 'myappsandbox';
            nameEl.dispatchEvent(new Event('input'));
            nameEl.focus();
        }
        document.documentElement.setAttribute('data-shot-ready', '1');
    }

    if (document.readyState === 'complete') pose();
    else window.addEventListener('load', pose);
})();
"""

# The viewer window draws live VNC framebuffers. For a screenshot there is no
# guest to connect to, so window.NoVNC is replaced with a stub that paints a
# placeholder desktop into each tile. Everything around the tiles -- the bar,
# the grid, the tile chrome and the focus ring -- is the real viewer.
DEMO_NOVNC_JS = r"""
(function() {
    var HUES = [212, 268, 156, 28];
    var seq = 0;

    /* A generic desktop: a panel, two desktop icons and one file-manager
       window. Deliberately says nothing about the guest -- it stands in for a
       framebuffer so the tiling is legible, it is not a picture of a real one. */
    function desktop(container, hue) {
        var folders = '';
        for (var i = 0; i < 8; i++) folders += '<div class="demo-file"><i></i><b></b></div>';
        container.innerHTML =
            '<div class="demo-desk" style="--h:' + hue + '">' +
              '<div class="demo-panel">' +
                '<span class="demo-app">Applications</span>' +
                '<span class="demo-sp"></span>' +
                '<span class="demo-clock">21:04</span>' +
              '</div>' +
              '<div class="demo-body">' +
                '<div class="demo-icons">' +
                  '<div class="demo-icon"><i></i><span>Home</span></div>' +
                  '<div class="demo-icon"><i></i><span>Trash</span></div>' +
                '</div>' +
                '<div class="demo-win">' +
                  '<div class="demo-win-bar"><span class="demo-dots"></span>File Manager</div>' +
                  '<div class="demo-win-body">' +
                    '<div class="demo-side"><u></u><u></u><u></u><u></u></div>' +
                    '<div class="demo-files">' + folders + '</div>' +
                  '</div>' +
                '</div>' +
              '</div>' +
            '</div>';
    }

    function StubRFB(container, url, opts) {
        var self = this;
        this._handlers = {};
        desktop(container, HUES[seq++ % HUES.length]);
        setTimeout(function() {
            (self._handlers['connect'] || []).forEach(function(fn) { fn({}); });
        }, 0);
    }
    StubRFB.prototype.addEventListener = function(type, fn) {
        (this._handlers[type] = this._handlers[type] || []).push(fn);
    };
    StubRFB.prototype.focus = function() {};
    StubRFB.prototype.blur = function() {};
    StubRFB.prototype.disconnect = function() {};
    StubRFB.prototype.sendCtrlAltDel = function() {};
    StubRFB.prototype.sendCredentials = function() {};

    window.NoVNC = Promise.resolve(StubRFB);

    var css = document.createElement('style');
    css.textContent = [
        /* .tile-screen is a plain flex child, so the stand-in desktop fills it
           by size rather than by anchoring to it. */
        '.demo-desk{width:100%;height:100%;display:flex;flex-direction:column;background:',
        '  radial-gradient(120% 100% at 30% 0%, hsl(var(--h) 42% 26%), hsl(var(--h) 46% 12%));',
        '  font:12px system-ui,sans-serif;color:#e8e8ee}',
        '.demo-panel{display:flex;align-items:center;gap:8px;height:26px;padding:0 10px;',
        '  background:rgba(12,13,18,.72);border-bottom:1px solid rgba(255,255,255,.07)}',
        '.demo-app{font-weight:600;font-size:11px;opacity:.9}',
        '.demo-sp{flex:1}',
        '.demo-clock{font:11px ui-monospace,monospace;opacity:.75}',
        '.demo-body{flex:1;min-height:0;display:flex;padding:14px;gap:18px}',
        '.demo-icons{display:flex;flex-direction:column;gap:14px;align-items:flex-start}',
        '.demo-icon{display:flex;flex-direction:column;align-items:center;width:56px;gap:5px;',
        '  font-size:10px;opacity:.85;text-shadow:0 1px 2px rgba(0,0,0,.6)}',
        '.demo-icon i{width:26px;height:22px;border-radius:3px;background:rgba(255,255,255,.22);',
        '  border:1px solid rgba(255,255,255,.3)}',
        '.demo-win{flex:1;min-width:0;max-width:420px;height:200px;align-self:flex-start;',
        '  margin-top:10px;border-radius:5px;overflow:hidden;background:#23242b;',
        '  border:1px solid rgba(255,255,255,.13);box-shadow:0 10px 26px rgba(0,0,0,.42);',
        '  display:flex;flex-direction:column}',
        '.demo-win-bar{display:flex;align-items:center;gap:8px;height:24px;padding:0 8px;',
        '  font-size:11px;background:#2e303a;border-bottom:1px solid rgba(255,255,255,.09)}',
        '.demo-dots{width:26px;height:8px;border-radius:4px;background:rgba(255,255,255,.2)}',
        '.demo-win-body{flex:1;min-height:0;display:flex}',
        '.demo-side{width:64px;padding:8px 6px;display:flex;flex-direction:column;gap:8px;',
        '  background:#1e1f26;border-right:1px solid rgba(255,255,255,.07)}',
        '.demo-side u{height:7px;border-radius:3px;background:rgba(255,255,255,.13)}',
        '.demo-files{flex:1;padding:10px;display:grid;grid-template-columns:repeat(4,1fr);',
        '  gap:10px;align-content:start}',
        '.demo-file{display:flex;flex-direction:column;align-items:center;gap:4px}',
        '.demo-file i{width:24px;height:19px;border-radius:2px;background:hsl(var(--h) 40% 52%/.65)}',
        '.demo-file b{width:28px;height:5px;border-radius:3px;background:rgba(255,255,255,.16)}'
    ].join('\n');
    document.head.appendChild(css);
})();
"""

SHOTS = [
    ("nestbox-main.png", "index.html?shot=main", WIDTH, HEIGHT),
    ("nestbox-light.png", "index.html?shot=light", WIDTH, HEIGHT),
    ("nestbox-new-sandbox.png", "index.html?shot=new-sandbox", WIDTH, HEIGHT),
    ("nestbox-grid.png",
     "viewer.html?grid=1&vm=1&vmName=myappsandbox"
     "&tiles=replica:5901:5900,steam-2:5902:5900,build-3:5903:5900,ci-runner:5904:5900",
     VIEWER_W, VIEWER_H),
]


def find_browser():
    for path in BROWSERS:
        if os.path.exists(path):
            return path
    sys.exit("no Edge or Chrome found; install one or edit BROWSERS")


def stage(tmp):
    """web/ plus the two demo scripts, with the pages wired to load them."""
    for name in os.listdir(WEB):
        src = os.path.join(WEB, name)
        if os.path.isfile(src):
            shutil.copy2(src, os.path.join(tmp, name))

    open(os.path.join(tmp, "demo-state.js"), "w", encoding="utf-8").write(DEMO_JS)
    open(os.path.join(tmp, "demo-novnc.js"), "w", encoding="utf-8").write(DEMO_NOVNC_JS)

    index = os.path.join(tmp, "index.html")
    html = open(index, encoding="utf-8").read()
    html = html.replace("</body>", '<script src="demo-state.js"></script>\n</body>')
    open(index, "w", encoding="utf-8").write(html)

    viewer = os.path.join(tmp, "viewer.html")
    html = open(viewer, encoding="utf-8").read()
    # After novnc.js so the stub wins, before viewer.js so it is in place when
    # the viewer connects.
    html = html.replace('<script src="viewer.js">',
                        '<script src="demo-novnc.js"></script>\n<script src="viewer.js">')
    open(viewer, "w", encoding="utf-8").write(html)


def shoot(browser, tmp, name, page, w, h):
    out = os.path.join(OUT, name)
    subprocess.run([
        browser, "--headless=new", "--disable-gpu", "--hide-scrollbars",
        "--window-size=%d,%d" % (w, h),
        "--virtual-time-budget=4000",
        "--screenshot=" + out,
        "file:///" + os.path.join(tmp, page).replace("\\", "/"),
    ], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    kb = shrink(out)
    print("%-26s %5d x %-4d %5d KB  %s" % (name, w, h, kb, page.split("?")[0]))


def shrink(path):
    """A UI screenshot is flat colour and gradients: 256 indexed colours are
    indistinguishable here and cut the file to roughly a third."""
    from PIL import Image
    im = Image.open(path).convert("RGB")
    im.quantize(colors=256, method=Image.MEDIANCUT,
                dither=Image.FLOYDSTEINBERG).save(path, optimize=True)
    return os.path.getsize(path) // 1024


def main():
    browser = find_browser()
    with tempfile.TemporaryDirectory() as tmp:
        stage(tmp)
        for name, page, w, h in SHOTS:
            shoot(browser, tmp, name, page, w, h)
    print("-> " + OUT)


if __name__ == "__main__":
    main()
