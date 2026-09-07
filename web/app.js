/* Nestbox - WebView2 Frontend */
'use strict';

/* ---- State ---- */
let vms = [];
let selectedVm = -1;
let selectedSnap = {};  /* vmIndex -> string value: 'current', 'base', 'base-N', 'S', 'S-N' */
let editModeRow = -1;
let editingCell = null; /* {row, col, element} */
let pendingConfirm = null; /* {resolve} */
let minSizeReported = false;
let lastHostInfo = null;
let rowCache = {};          /* vm.name -> <tr> — persistent rows so the status spinner doesn't reset on every update */
let rowSigCache = {};       /* vm.name -> last render signature; skip rebuild when unchanged */

let firstStateSeen = false;   /* the host's first state message: drops the skeleton rows */

/* ---- Pending: what the page is waiting on ----
   Every command goes to the host and comes back as a state change some time
   later - a Start, seconds; a seat create, a minute; a replica create,
   twenty. Until then the row has to say what is happening. An entry is
   keyed by what it is about ('vm:<name>', 'rep:<vm>/<name>',
   'create:rep:<vm>/<name>', 'create:vm:<name>', 'ident:<vm>', 'snap:<vm>',
   'edit:<vm>', 'tpl:<name>'), carries the label to show, and clears itself
   when expect() sees the state it waited for, on a log line that says the
   step failed (kept visible as "failed" for a moment), or at `until` at the
   latest. */
var pending = {};
var pendingTimer = null;

function setPending(key, opts) {
    var now = Date.now();
    pending[key] = { label: opts.label, name: opts.name || '', since: now,
                     until: now + (opts.ttl || 90000), expect: opts.expect || null,
                     col: opts.col, kind: opts.kind || '', doneOnLog: !!opts.doneOnLog, failed: '' };
    renderVmTable();
    schedulePendingTick();
}
function pendingFor(key) {
    var p = pending[key];
    if (!p) return null;
    if (Date.now() > p.until) { delete pending[key]; return null; }
    return p;
}
function clearPending(key) { if (pending[key]) { delete pending[key]; renderVmTable(); } }
/* drop what has arrived or timed out; true when something changed */
function resolvePending() {
    var now = Date.now(), changed = false;
    Object.keys(pending).forEach(function(k) {
        var p = pending[k];
        if (now > p.until || (!p.failed && p.expect && p.expect())) { delete pending[k]; changed = true; }
    });
    return changed;
}
/* one tick a second while something is pending: the elapsed counters on the
   placeholder rows update in place, expired entries go away */
function schedulePendingTick() {
    if (pendingTimer) return;
    pendingTimer = setInterval(function() {
        var keys = Object.keys(pending);
        if (!keys.length) { clearInterval(pendingTimer); pendingTimer = null; return; }
        var now = Date.now(), expired = false;
        keys.forEach(function(k) { if (now > pending[k].until) expired = true; });
        if (expired) { renderVmTable(); return; }
        document.querySelectorAll('[data-pending-elapsed]').forEach(function(el) {
            var p = pending[el.getAttribute('data-pending-elapsed')];
            if (p) el.textContent = elapsedText(now - p.since);
        });
    }, 1000);
}
function elapsedText(ms) {
    var s = Math.max(0, Math.floor(ms / 1000)), m = Math.floor(s / 60);
    return m ? m + ':' + (s % 60 < 10 ? '0' : '') + (s % 60) : s + 's';
}
/* the pending entries that touch one VM (its own, its replicas and seats),
   for the row's render signature */
function pendingSig(vmName) {
    return Object.keys(pending).filter(function(k) {
        return k.slice(-vmName.length - 1) === ':' + vmName || k.indexOf(':' + vmName + '/') >= 0;
    }).map(function(k) { return k + '=' + pending[k].label + (pending[k].failed ? '!' : ''); }).join(';');
}
/* A log line that names a pending thing and says it failed ends the wait:
   the hosts report failures only in the log ("…:create:failed.", "[seat2]
   creating the Steam seat failed (rc=1).", "busy, try again", "already
   exists"). A line that says a step is done ("…:restart:ok.", "[seat2]
   done.") ends the waits that have no state to look for (a restart). */
function pendingFromLog(msg) {
    var hit = false;
    Object.keys(pending).forEach(function(k) {
        var p = pending[k];
        if (!p.name || p.failed || msg.indexOf(p.name) < 0) return;
        if (/failed|busy, try again|already exists|not installed|is not online|cannot|error/i.test(msg)) {
            p.failed = msg; p.until = Date.now() + 8000; hit = true;
        } else if (p.doneOnLog && /:ok\.|\] done\./.test(msg)) {
            delete pending[k]; hit = true;
        }
    });
    if (hit) renderVmTable();
}
function findVm(name) { for (var i = 0; i < vms.length; i++) if (vms[i].name === name) return vms[i]; return null; }
function findRep(vmName, name) {
    var vm = findVm(vmName); if (!vm) return null;
    var reps = parseReplicas(vm.replicas);
    for (var i = 0; i < reps.length; i++) if (reps[i].name === name) return reps[i];
    return null;
}
/* Linux host: the WebSocket to nestbox is the page's only link to anything */
function setConnBanner(text) {
    var b = document.getElementById('conn-banner');
    if (!b) return;
    b.hidden = !text;
    if (text) document.getElementById('conn-text').textContent = text;
}

/* ---- Collapsible sections ---- */
/* ---- Theme: dark by default, remembered per machine ---- */
function applyTheme(theme) {
    document.documentElement.setAttribute('data-theme', theme === 'light' ? 'light' : 'dark');
    var use = document.querySelector('#theme-icon use');
    if (use) use.setAttribute('href', theme === 'light' ? '#i-moon' : '#i-sun');
}
function toggleTheme() {
    var next = document.documentElement.getAttribute('data-theme') === 'light' ? 'dark' : 'light';
    applyTheme(next);
    try { localStorage.setItem('asb-theme', next); } catch (e) {}
}
(function() {
    var t = null;
    try { t = localStorage.getItem('asb-theme'); } catch (e) {}
    if (t) applyTheme(t);
})();

function toggleSection(id) {
    var section = document.getElementById(id);
    var collapsed = section.classList.toggle('collapsed');
    localStorage.setItem('collapse_' + id, collapsed ? '1' : '0');
}
(function restoreCollapse() {
    var defaults = { 'log-section': '1' };
    Object.keys(defaults).forEach(function(id) {
        var val = localStorage.getItem('collapse_' + id);
        if (val === null) val = defaults[id];
        if (val === '1') document.getElementById(id).classList.add('collapsed');
    });
})();

const netNames = ['None', 'NAT', 'External', 'Internal'];

/* ---- Message bridge ----
 *
 * Two host environments are supported:
 *   - WebView2 on Windows  (window.chrome.webview)
 *   - WKWebView on macOS   (window.webkit.messageHandlers.host)
 *
 * Native code on both platforms calls window.onHostMessage(obj) with a
 * parsed message object; the JS side only sees one uniform surface. On
 * Windows we keep using the native chrome.webview event path because it
 * is the existing, tested route — onHostMessage is simply wired into the
 * same listener.
 */

var hostBridge = (function() {
    var isWebView2 = !!(window.chrome && window.chrome.webview);
    var isWKWebView = !!(window.webkit && window.webkit.messageHandlers && window.webkit.messageHandlers.host);
    /* Linux host: the page is served by tools/linux/host/nestbox over HTTP and
     * talks to it on a WebSocket at /ws with the same JSON messages. */
    var isWS = !isWebView2 && !isWKWebView && /^https?:/.test(location.protocol);
    var ws = null, wsQueue = [];

    function wsConnect() {
        ws = new WebSocket((location.protocol === 'https:' ? 'wss://' : 'ws://') + location.host + '/ws');
        ws.onopen = function() {
            if (window.setConnBanner) setConnBanner(null);
            var q = wsQueue; wsQueue = [];
            q.forEach(function(s) { ws.send(s); });
        };
        ws.onmessage = function(ev) {
            var m = null;
            try { m = JSON.parse(ev.data); } catch (e) {}
            if (m && window.onHostMessage) window.onHostMessage(m);
        };
        ws.onclose = function() {
            ws = null;
            if (window.appendLog) appendLog('Lost the connection to nestbox; reconnecting...');
            if (window.setConnBanner) setConnBanner('Lost the connection to nestbox. Reconnecting…');
            setTimeout(function() { wsConnect(); send('uiReady'); }, 2000);
        };
    }

    function send(action, data) {
        var msg = Object.assign({ action: action }, data || {});
        if (isWebView2) {
            window.chrome.webview.postMessage(msg);
        } else if (isWKWebView) {
            /* WKWebView only accepts JSON-serializable values; strings round-trip
             * most reliably so we hand the native side the raw JSON text. */
            window.webkit.messageHandlers.host.postMessage(JSON.stringify(msg));
        } else if (isWS) {
            var s = JSON.stringify(msg);
            if (ws && ws.readyState === 1) ws.send(s);
            else wsQueue.push(s);
        } else {
            console.warn('[hostBridge] no native host available; dropping', msg);
        }
    }

    if (isWS) wsConnect();
    return { send: send, isWebView2: isWebView2, isWKWebView: isWKWebView, isMac: isWKWebView,
             isLinux: isWS, noSnapshots: isWKWebView || isWS };
})();

function sendCmd(action, data) { hostBridge.send(action, data); }

/* On a macOS host, hide the Windows-*host*-only features (templates, snapshots,
 * test-mode, build-template — none supported when the host is a Mac) and the
 * dormant Linux-version row. The OS-type dropdown stays ENABLED so the user can
 * pick Windows (built from a Microsoft ISO via QEMU) or macOS (VZ restore image).
 * Per-OS field visibility — including the .needs-iso picker — is driven by
 * applyOsTypeUI(), which runs on both hosts. */
if (hostBridge.isMac) {
    var hide = document.querySelectorAll('.win-only, .needs-linux-version, .snap-col');
    for (var i = 0; i < hide.length; i++) hide[i].style.display = 'none';
}

/* On a Linux host there are no sandbox VMs at all: this PC is the top row and
 * the replicas run on it directly, so New Sandbox and the Windows-host
 * columns go away. */
if (hostBridge.isLinux) {
    var hideL = document.querySelectorAll('.win-only, .needs-linux-version, .snap-col, #btn-new-sandbox');
    for (var li = 0; li < hideL.length; li++) hideL[li].style.display = 'none';
    document.title = 'Nestbox';
}

/* OS Type dropdown: drop guest types that aren't available on this host.
 * Windows host: macOS unavailable (Apple Virtualization is Mac-only).
 * macOS host:   Linux unavailable (Windows IS supported — QEMU+ivshmem). */
{
    var unavailable = hostBridge.isMac ? ['Linux'] : ['macOS'];
    unavailable.forEach(function(v) {
        var opt = document.querySelector('#os-type option[value="' + v + '"]');
        if (opt) opt.remove();
    });
}

/* Apply Create-modal visibility rules for the currently selected OS type.
 *   Windows: .win-only shown, .needs-iso shown,             .needs-linux-version hidden
 *   Linux:   .win-only hidden, .needs-iso hidden,           .needs-linux-version shown
 *   macOS:   handled by the isMac branch above; this function is a no-op there.
 *
 * Linux is back to user-picks-an-ISO (Ubuntu Desktop ISO etc.), same as
 * Windows. The version-dropdown / cloud-image flow is preserved in
 * asb_core.c under #if 0 in case we need to bring it back. */
function applyOsTypeUI() {
    var osType = document.getElementById('os-type').value;
    var isWindows = osType === 'Windows';
    var isLinux = osType === 'Linux';
    var winOnly = document.querySelectorAll('.win-only');
    var needsIso = document.querySelectorAll('.needs-iso');
    var needsWindows = document.querySelectorAll('.needs-windows');
    var needsLinux = document.querySelectorAll('.needs-linux');
    var needsLinuxVersion = document.querySelectorAll('.needs-linux-version');
    /* .win-only = template/snapshot features that exist only on a Windows *host*;
       never shown on a Mac host, even for a Windows guest. */
    for (var i = 0; i < winOnly.length; i++)
        winOnly[i].style.display = (!hostBridge.isMac && isWindows) ? '' : 'none';
    /* .needs-windows = Windows-*guest* options (Test Mode); shown for a Windows
       guest on EITHER host (a Windows-on-Mac VM uses it too), hidden otherwise. */
    for (var w = 0; w < needsWindows.length; w++) needsWindows[w].style.display = isWindows ? '' : 'none';
    /* .needs-linux = Linux-guest options (GA kernel). */
    for (var l = 0; l < needsLinux.length; l++) needsLinux[l].style.display = isLinux ? '' : 'none';
    /* ISO picker shows for both Windows and Linux now. */
    for (var j = 0; j < needsIso.length; j++) needsIso[j].style.display = (isWindows || isLinux) ? '' : 'none';
    /* Linux distribution dropdown is dormant — kept in the DOM but always
       hidden so the cloud-image code path can be revived without
       re-adding the markup. */
    for (var k = 0; k < needsLinuxVersion.length; k++) needsLinuxVersion[k].style.display = 'none';
    /* Swap the default VM name between OS conventions, but only when the
       field still holds the *other* OS's untouched default — never clobber a
       name the user typed. Linux hostnames must be lowercase. */
    var nameEl = document.getElementById('vm-name');
    if (isLinux && nameEl.value === 'MyAppSandbox') nameEl.value = 'myappsandbox';
    else if (!isLinux && nameEl.value === 'myappsandbox') nameEl.value = 'MyAppSandbox';
    revalidateVmName();
    revalidateUsername();
    revalidatePassword();
    updateCreateButtons();
}

/* Unified dispatch. Native code on either platform calls
 * window.onHostMessage(obj) with an already-parsed object. WebView2 also
 * delivers messages through chrome.webview.addEventListener('message'),
 * which we route into the same handler so both paths end up in one place. */
window.onHostMessage = function(msg) {
    if (!msg || typeof msg !== 'object') return;
    switch (msg.type) {
        case 'fullState':     firstStateSeen = true; onFullState(msg); break;
        case 'vmListChanged': firstStateSeen = true; vms = msg.vms; renderVmTable(); updateHostInfo(msg.hostInfo); revalidateVmName(); break;
        case 'vmStateChanged': firstStateSeen = true; onVmStateChanged(msg); break;
        case 'snapListChanged': break; /* snapshots now inline in vmListChanged */
        case 'log':           appendLog(msg.message); pendingFromLog(msg.message); break;
        case 'hostInfo':      updateHostInfo(msg); break;
        case 'browseResult':  onBrowseResult(msg.path); break;
        case 'confirmResult': if (pendingConfirm) pendingConfirm.resolve(msg.confirmed); break;
        case 'adapters':      populateAdapters(msg.adapters, msg.defaultIndex); break;
        case 'templates':     populateTemplates(msg.templates); break;
        case 'identity':      if (vms[msg.vmIndex]) delete pending['ident:' + vms[msg.vmIndex].name]; openIdentityModal(msg.vmIndex, msg.vmIdentity); break;
        case 'alert':         showModal('Error', msg.message, 'OK'); break;
        case 'openWindow':    openHostWindow(msg); break;
        case 'prereqRequired': onPrereqRequired(); break;
        case 'prereqReboot':   onPrereqReboot(); break;
        case 'prereqProgress': onPrereqProgress(msg); break;
        case 'prereqResult':   onPrereqResult(msg); break;
    }
};

/* WebView2 delivers events as DOM CustomEvents; forward them into
 * window.onHostMessage so both transports converge on the same handler. */
if (hostBridge.isWebView2) {
    window.chrome.webview.addEventListener('message', function(event) {
        window.onHostMessage(event.data);
    });
}

/* ---- Initial state ---- */

function onFullState(msg) {
    vms = msg.vms || [];
    renderVmTable();
    revalidateVmName();
    if (msg.hostInfo) updateHostInfo(msg.hostInfo);
    if (msg.adapters) populateAdapters(msg.adapters, msg.defaultAdapter);
    if (msg.templates) populateTemplates(msg.templates);
    if (!minSizeReported) {
        minSizeReported = true;
        setTimeout(reportMinSize, 50);
    }
}

/* Hyper-V/HCS requires VM memory aligned to 2 MB, so RAM (MB) must be even;
   round an odd value down by 1 (an odd value is rejected and the VM won't boot). */
function alignRamMb(mb) { return mb - (mb % 2); }

function applySmartDefaults(info) {
    var ram = Math.min(Math.floor(info.hostRamMb / 2), 16384);
    var cores = Math.min(Math.floor(info.hostCores / 2), 8);
    if (ram < 512) ram = 512;
    if (cores < 1) cores = 1;
    document.getElementById('ram-size').value = alignRamMb(ram);
    document.getElementById('cpu-cores').value = cores;
}

function onVmStateChanged(msg) {
    if (msg.vmIndex >= 0 && msg.vmIndex < vms.length) {
        Object.assign(vms[msg.vmIndex], msg);
    }
    renderVmTable();
    if (msg.hostInfo) updateHostInfo(msg.hostInfo);
}

/* ---- Host info ---- */

function updateHostInfo(info) {
    if (!info) return;
    lastHostInfo = info;
    var el;
    el = document.getElementById('host-cpu');
    if (el) el.textContent = 'Host: ' + info.hostCores + ' cores | VMs using: ' + info.vmCores;
    el = document.getElementById('host-ram');
    if (el) el.textContent = 'Host: ' + info.hostRamMb + ' MB | VMs using: ' + info.vmRamMb + ' MB';
    el = document.getElementById('host-hdd');
    if (el) el.textContent = 'Free: ' + info.freeGb + ' GB | VMs allocated: ' + info.vmHddGb + ' GB';
    el = document.getElementById('host-strip');
    if (el) el.textContent = info.hostCores + ' cores · ' + Math.round(info.hostRamMb / 1024) + ' GB RAM · ' + info.freeGb + ' GB free';
}

/* ---- Adapters ---- */

var currentAdapters = [];
var currentDefaultAdapter = '';

function populateAdapters(adapters, defaultIdx) {
    var sel = document.getElementById('net-adapter');
    sel.innerHTML = '<option value="">(Auto)</option>';
    currentAdapters = adapters || [];
    if (adapters) {
        adapters.forEach(function(a) {
            var opt = document.createElement('option');
            opt.value = a;
            opt.textContent = a;
            sel.appendChild(opt);
        });
    }
    if (typeof defaultIdx === 'number' && defaultIdx >= 0 && defaultIdx < sel.options.length) {
        sel.selectedIndex = defaultIdx;
        currentDefaultAdapter = sel.value;
    } else if (adapters && adapters.length > 0) {
        currentDefaultAdapter = adapters[0];
    }
}

/* ---- Templates ---- */

var currentTemplates = [];

function templateDefaultLabel() {
    var n = currentTemplates.length;
    if (n === 0) return '(None)';
    return '(' + n + ' template' + (n === 1 ? '' : 's') + ' available)';
}

function populateTemplates(templates) {
    currentTemplates = templates || [];
    var list = document.getElementById('template-dropdown-list');
    var hidden = document.getElementById('template-select');
    list.innerHTML = '';

    /* Default (None) item — always shows "None" inside the list */
    var noneItem = document.createElement('div');
    noneItem.className = 'template-dropdown-item';
    noneItem.innerHTML = '<span class="tpl-name">(None)</span>';
    noneItem.addEventListener('click', function() { selectTemplate('', templateDefaultLabel()); });
    list.appendChild(noneItem);

    currentTemplates.forEach(function(t) {
        var item = document.createElement('div');
        item.className = 'template-dropdown-item';

        var nameSpan = document.createElement('span');
        nameSpan.className = 'tpl-name';
        nameSpan.textContent = t.name + ' [' + t.osType + ']';
        item.appendChild(nameSpan);

        var tp = pendingFor('tpl:' + t.name);
        if (tp) {
            item.classList.add('pending');
            var sp = document.createElement('span');
            sp.className = 'spinner';
            sp.title = 'Deleting…';
            item.appendChild(sp);
            list.appendChild(item);
            return;
        }
        var delBtn = document.createElement('span');
        delBtn.className = 'tpl-delete';
        delBtn.innerHTML = '<svg class="ic"><use href="#i-trash"/></svg>';
        delBtn.title = 'Delete template';
        delBtn.addEventListener('click', function(e) {
            e.stopPropagation();
            closeTemplateDropdown();
            onDeleteTemplate(t.name);
        });
        item.appendChild(delBtn);

        item.addEventListener('click', function() {
            selectTemplate(t.name, t.name + ' [' + t.osType + ']');
        });
        list.appendChild(item);
    });

    /* If the currently selected template was deleted, reset */
    if (hidden.value !== '') {
        var found = currentTemplates.some(function(t) { return t.name === hidden.value; });
        if (!found) selectTemplate('', templateDefaultLabel());
    } else {
        /* No template selected — update default label in case count changed */
        document.getElementById('template-dropdown-selected').textContent = templateDefaultLabel();
    }
}

function selectTemplate(value, label) {
    document.getElementById('template-select').value = value;
    document.getElementById('template-dropdown-selected').textContent = label;
    closeTemplateDropdown();
    if (value !== '') {
        document.getElementById('image-path').value = '';
    }
    updateCreateButtons();
}

function closeTemplateDropdown() {
    document.getElementById('template-dropdown').classList.remove('open');
}

document.getElementById('template-dropdown-selected').addEventListener('click', function() {
    document.getElementById('template-dropdown').classList.toggle('open');
});

/* Close dropdown when clicking outside */
document.addEventListener('click', function(e) {
    if (!e.target.closest('#template-dropdown')) {
        closeTemplateDropdown();
    }
});

function onDeleteTemplate(name) {
    showModal(
        'Confirm Delete',
        'Are you sure you want to delete template "' + name + '"?\n\nThis will permanently delete the template disk image.',
        'Delete'
    ).then(function(confirmed) {
        if (confirmed) {
            setPending('tpl:' + name, { label: 'Deleting', name: name, ttl: 60000, expect: function() {
                return !currentTemplates.some(function(t) { return t.name === name; });
            } });
            sendCmd('deleteTemplate', { name: name });
        }
    });
}

/* ---- Browse result ---- */

function onBrowseResult(path) {
    if (path) {
        document.getElementById('image-path').value = path;
        selectTemplate('', templateDefaultLabel());
        updateCreateButtons();
    }
}

/* ---- Create buttons state ---- */

function updateCreateButtons() {
    var osType = document.getElementById('os-type').value;
    var hasImage = (document.getElementById('image-path').value.trim() !== '');
    var hasTpl = document.getElementById('template-select').value !== '';
    /* macOS guests auto-download their restore image (no path needed). Windows
       and Linux guests build from a user-picked ISO — or, on a Windows host, a
       saved template. Holds on both hosts: on a Mac the template UI is hidden so
       hasTpl stays false and a Windows guest genuinely requires the ISO. */
    var createOk = (osType === 'macOS') ? true : (hasImage || hasTpl);
    document.getElementById('btn-create').disabled = !createOk;
    /* Templates are Windows-only; disabling create-as-template for Linux
       (and macOS) is fine since hasImage is the only signal we check. */
    document.getElementById('btn-create-template').disabled = (osType !== 'Windows') || !hasImage;
}

/* Wire up change events */
document.getElementById('image-path').addEventListener('input', function() {
    if (this.value.trim() !== '') {
        selectTemplate('', templateDefaultLabel());
    }
    updateCreateButtons();
});

/* RAM must be 2 MB-aligned: snap an odd entry down by 1 when the field is committed. */
document.getElementById('ram-size').addEventListener('change', function() {
    var mb = parseInt(this.value, 10);
    if (!isNaN(mb)) this.value = alignRamMb(mb);
});

function revalidateVmName() {
    var name = document.getElementById('vm-name').value.trim();
    document.getElementById('vm-name-warn').textContent = validateVmName(name) || '';
}
document.getElementById('vm-name').addEventListener('input', revalidateVmName);

function revalidateUsername() {
    var u = document.getElementById('admin-user').value.trim();
    document.getElementById('admin-user-warn').textContent = validateUsername(u) || '';
}
function revalidatePassword() {
    var p = document.getElementById('admin-pass').value;
    document.getElementById('admin-pass-warn').textContent = validatePassword(p) || '';
}
document.getElementById('admin-user').addEventListener('input', revalidateUsername);

function checkPasswordMatch() {
    var pass = document.getElementById('admin-pass').value;
    var confirm = document.getElementById('admin-confirm');
    if (confirm.value === '' && pass === '') {
        confirm.classList.remove('pass-mismatch', 'pass-match');
        return;
    }
    if (confirm.value === pass) {
        confirm.classList.remove('pass-mismatch');
        confirm.classList.add('pass-match');
    } else {
        confirm.classList.remove('pass-match');
        confirm.classList.add('pass-mismatch');
    }
}
document.getElementById('admin-pass').addEventListener('input', function() {
    checkPasswordMatch();
    revalidatePassword();
});
document.getElementById('admin-confirm').addEventListener('input', checkPasswordMatch);
checkPasswordMatch();

function showPassword() {
    document.getElementById('admin-pass').type = 'text';
    document.getElementById('admin-confirm').type = 'text';
}
function hidePassword() {
    document.getElementById('admin-pass').type = 'password';
    document.getElementById('admin-confirm').type = 'password';
}

function onNetModeChange() {
    /* Adapter dropdown only relevant for External */
    var mode = parseInt(document.getElementById('net-mode').value);
    var show = (mode === 2) ? '' : 'none';
    document.getElementById('net-adapter').style.display = show;
    document.getElementById('net-adapter-label').style.display = show;
}
onNetModeChange();

/* ---- Create VM ---- */

function gatherConfig() {
    var osType = document.getElementById('os-type').value;
    /* Same ISO-picker path for Windows and Linux. The cloud-image
       Linux-version dropdown is dormant (see applyOsTypeUI). */
    var imagePath = document.getElementById('image-path').value.trim();
    return {
        name:        document.getElementById('vm-name').value.trim(),
        osType:      osType,
        imagePath:   imagePath,
        templateName: document.getElementById('template-select').value,
        hddGb:       parseInt(document.getElementById('hdd-size').value) || 64,
        ramMb:       alignRamMb(parseInt(document.getElementById('ram-size').value) || 16384),
        cpuCores:    parseInt(document.getElementById('cpu-cores').value) || 8,
        gpuMode:     parseInt(document.getElementById('gpu-mode').value),
        networkMode: parseInt(document.getElementById('net-mode').value),
        netAdapter:  document.getElementById('net-adapter').value,
        adminUser:   document.getElementById('admin-user').value.trim(),
        adminPass:   document.getElementById('admin-pass').value,
        adminConfirm: document.getElementById('admin-confirm').value,
        testMode:    document.getElementById('test-mode').checked,
        sshEnabled:  document.getElementById('ssh-enabled').checked,
        sshDeployKey: document.getElementById('ssh-deploy-key').checked,
        gaKernel:    document.getElementById('ga-kernel').checked,
        replicaAuto: document.getElementById('replica-auto').checked,
        vmIdentity:  compactIdentity(document.getElementById('vm-identity').value)
    };
}

/* ---- Screens ----
   vncOpen makes the host open a window of its own (web/viewer.html on a
   separate WebView2) on a loopback WebSocket bridge onto the VNC tunnel -
   the nested replica's console, or the guest's VNC server on port 5900.
   Nothing comes back to this page on Windows; the Linux host cannot open
   windows itself and answers with openWindow {url} instead, which the
   browser turns into a popup. */
function openHostWindow(msg) {
    var feat = 'popup=yes';
    if (msg.width && msg.height) feat += ',width=' + msg.width + ',height=' + msg.height;
    else feat += ',width=' + Math.max(800, screen.availWidth - 80) + ',height=' + Math.max(600, screen.availHeight - 120);
    var w = window.open(msg.url, '_blank', feat);
    if (!w) appendLog('The browser blocked the screen window; allow popups for this page and try again.');
}

/* ---- VM identity editor (per-VM modal) ---- */
var identityVmIndex = -1;

/* Default VM identity for a new Linux sandbox: a bare-metal ASUS/AMI desktop
   (the literal strings such boards ship, so nothing looks invented). The
   guest overlay takes the DMI keys + systemd-detect-virt; the nested replica
   takes those plus the ACPI / SMBIOS / drive / USB / CPUID keys with its
   identity-patched QEMU. Same profile as tools/linux/identity/README.md.
   Clear the field to create a VM that reports its real (Hyper-V) identity. */
var DEFAULT_VM_IDENTITY = [
    {check: 'sys_vendor',          name: 'ASUS'},
    {check: 'product_name',        name: 'System Product Name'},
    {check: 'product_version',     name: 'System Version'},
    {check: 'product_serial',      name: 'System Serial Number'},
    {check: 'product_family',      name: 'To be filled by O.E.M.'},
    {check: 'bios_vendor',         name: 'American Megatrends Inc.'},
    {check: 'bios_version',        name: '2604'},
    {check: 'bios_date',           name: '01/15/2024'},
    {check: 'board_vendor',        name: 'ASUSTeK COMPUTER INC.'},
    {check: 'board_name',          name: 'ROG STRIX Z690-F GAMING WIFI'},
    {check: 'board_version',       name: 'Rev 1.xx'},
    {check: 'board_serial',        name: '230712345678901'},
    {check: 'chassis_vendor',      name: 'Default string'},
    {check: 'chassis_type',        name: '3'},
    {check: 'chassis_version',     name: 'Default string'},
    {check: 'chassis_serial',      name: 'Default string'},
    {check: 'systemd-detect-virt', name: 'none'},
    {check: 'acpi_oem_id',         name: 'ALASKA'},
    {check: 'acpi_oem_table_id',   name: 'A M I'},
    {check: 'acpi_creator_id',     name: 'AMI'},
    {check: 'smbios_manufacturer', name: 'Intel(R) Corporation'},
    {check: 'drive_vendor',        name: 'HL-DT-ST'},
    {check: 'cdrom_model',         name: 'DVDRAM GH24NSD1'},
    {check: 'disk_model',          name: 'Samsung SSD 870 EVO 1TB'},
    {check: 'usb_vendor',          name: 'Logitech'},
    {check: 'CPUID 0x40000000',    name: 'GenuineIntel'},
    {check: 'hypervisor CPU flag', name: 'not set'},
    {check: 'smbios_vm_bit',       name: 'not set'}
];

/* One {check,name} entry per line: readable in a textarea, still valid JSON. */
function defaultIdentityText() {
    return '[\n' + DEFAULT_VM_IDENTITY.map(function(e) { return '  ' + JSON.stringify(e); }).join(',\n') + '\n]';
}

function fillDefaultIdentity(id) {
    document.getElementById(id).value = defaultIdentityText();
}

function openIdentityModal(idx, json) {
    identityVmIndex = idx;
    document.getElementById('identity-vm-name').textContent = (vms[idx] && vms[idx].name) || '';
    var pretty = '';
    if (json) { try { pretty = JSON.stringify(JSON.parse(json), null, 2); } catch (e) { pretty = json; } }
    document.getElementById('identity-text').value = pretty;
    document.getElementById('identity-overlay').classList.add('active');
    setTimeout(function() { document.getElementById('identity-text').focus(); }, 50);
}

function closeIdentityModal() {
    document.getElementById('identity-overlay').classList.remove('active');
    identityVmIndex = -1;
}

function saveIdentity(clear) {
    var idx = identityVmIndex;
    var text = clear ? '' : document.getElementById('identity-text').value;
    var compact;
    try { compact = compactIdentity(text); }
    catch (e) { showModal('VM identity', e.message, 'OK', { confirmClass: 'primary' }); return; }
    var vmName = vms[idx] && vms[idx].name;
    if (vmName) pendVm(vmName, clear ? 'Clearing the identity' : 'Saving the identity', 15000, function() {
        var v = findVm(vmName); return !v || v.hasIdentity === !!compact;
    });
    sendCmd('setIdentity', { vmIndex: idx, vmIdentity: compact });
    closeIdentityModal();
}

/* VM identity profile: accept the [{check,name}] list or a {key:value}
   object, re-emit it compact (single line) for the config file, or '' when
   empty. Throws with a readable message on invalid JSON. */
function compactIdentity(text) {
    text = (text || '').trim();
    if (!text) return '';
    var v;
    try { v = JSON.parse(text); } catch (e) { throw new Error('VM identity is not valid JSON: ' + e.message); }
    if (Array.isArray(v)) {
        for (var i = 0; i < v.length; i++)
            if (!v[i] || typeof v[i] !== 'object' || !('check' in v[i]))
                throw new Error('VM identity: entry ' + (i + 1) + ' needs a "check" and a "name"');
    } else if (!v || typeof v !== 'object') {
        throw new Error('VM identity must be a JSON array of {"check","name"} or an object');
    }
    return JSON.stringify(v);
}

/* "Deploy SSH key" depends on "SSH Server": grey it out (and clear it) unless
   SSH is enabled. The core also gates deploy on ssh_enabled as a backstop. */
function onSshToggle() {
    var ssh = document.getElementById('ssh-enabled').checked;
    var dep = document.getElementById('ssh-deploy-key');
    dep.disabled = !ssh;
    if (!ssh) dep.checked = false;
}

function clearCreateForm() {
    document.getElementById('image-path').value = '';
    selectTemplate('', templateDefaultLabel());
    updateCreateButtons();
}

/* VM name / hostname validation. Per-guest-OS rules, keyed off the
   selected OS Type (on a macOS host the dropdown is locked to 'macOS',
   so osType is an accurate guest discriminator on all hosts). */
function validateVmName(name) {
    if (!name) return 'VM name is required.';
    var osSelect = document.getElementById('os-type');
    var osType = osSelect ? osSelect.value : 'Windows';
    if (osType === 'macOS') {
        if (name.length > 63) return 'VM name cannot exceed 63 characters (macOS LocalHostName limit).';
    } else if (osType === 'Linux') {
        if (name.length > 63) return 'VM name cannot exceed 63 characters (Linux hostname limit).';
        if (/[A-Z]/.test(name)) return 'Linux hostname must be lowercase.';
    } else { /* Windows */
        if (name.length > 15) return 'VM name cannot exceed 15 characters (NetBIOS limit).';
    }
    if (/[^a-zA-Z0-9-]/.test(name)) return 'VM name can only contain letters, digits, and hyphens.';
    if (/^\d+$/.test(name)) return 'VM name cannot be only digits.';
    if (name.startsWith('-') || name.endsWith('-')) return 'VM name cannot start or end with a hyphen.';
    var lower = name.toLowerCase();
    for (var i = 0; i < vms.length; i++) {
        if (vms[i].name.toLowerCase() === lower) return 'A VM with this name already exists.';
    }
    for (var j = 0; j < currentTemplates.length; j++) {
        if (currentTemplates[j].name.toLowerCase() === lower) return 'A template with this name already exists.';
    }
    return null;
}

/* Username validation. Per-guest-OS rules keyed off osType. Each branch
   is explicit so it's clear which OS's account rules apply. */
function validateUsername(name) {
    if (!name) return 'Username is required.';
    var osSelect = document.getElementById('os-type');
    var osType = osSelect ? osSelect.value : 'Windows';
    if (osType === 'Linux') {
        /* Ubuntu useradd/adduser: lowercase, start with a letter or
           underscore, then [a-z0-9_-], max 32 chars. */
        if (name.length > 32) return 'Username cannot exceed 32 characters (Linux limit).';
        if (!/^[a-z_][a-z0-9_-]*$/.test(name))
            return 'Lowercase alphanumeric only.';
        return null;
    }
    /* macOS and Windows: keep the existing Windows-account ruleset.
       (macOS-specific shortname rules are not yet verified; treated the
       same as Windows for now — see validatePassword note.) */
    if (name.length > 20) return 'Username cannot exceed 20 characters.';
    if (/["\\/\[\]:;|=,+*?<>]/.test(name)) return 'Username contains invalid characters.';
    if (/^[.\s]+$/.test(name)) return 'Username cannot be only dots or spaces.';
    if (name.endsWith('.')) return 'Username cannot end with a period.';
    var reserved = ['CON','PRN','AUX','NUL',
        'COM1','COM2','COM3','COM4','COM5','COM6','COM7','COM8','COM9',
        'LPT1','LPT2','LPT3','LPT4','LPT5','LPT6','LPT7','LPT8','LPT9'];
    if (reserved.indexOf(name.toUpperCase()) >= 0) return 'Username is a reserved name.';
    return null;
}

/* Password validation. Per-guest-OS rules keyed off osType.
   - Linux: Ubuntu accepts ALL characters via the host's $6$ hash path
     (usermod -p bypasses pwquality), so the only limits are non-empty
     and a sane byte ceiling.
   - macOS / Windows: no extra content rule enforced here today. */
function validatePassword(pass) {
    var osSelect = document.getElementById('os-type');
    var osType = osSelect ? osSelect.value : 'Windows';
    if (osType === 'Linux') {
        if (!pass) return 'Password is required.';
        /* UTF-8 byte length (encodeURIComponent escapes multibyte). */
        var bytes = unescape(encodeURIComponent(pass)).length;
        if (bytes > 255) return 'Password is too long (max 255 bytes).';
        return null;
    }
    /* macOS / Windows: no additional constraints today. */
    return null;
}

function onCreateVm() {
    var cfg = gatherConfig();
    var nameErr = validateVmName(cfg.name);
    if (nameErr) { sendCmd('log', { message: nameErr }); return; }
    var userErr = validateUsername(cfg.adminUser);
    if (userErr) { sendCmd('log', { message: userErr }); return; }
    var passErr = validatePassword(cfg.adminPass);
    if (passErr) { sendCmd('log', { message: passErr }); return; }
    if (cfg.adminPass !== cfg.adminConfirm) {
        sendCmd('log', { message: 'Passwords do not match.' });
        return;
    }
    setPending('create:vm:' + cfg.name, { label: 'Creating', name: cfg.name, kind: cfg.osType, ttl: 10 * 60000,
        expect: function() { return !!findVm(cfg.name); } });
    sendCmd('createVm', cfg);
    clearCreateForm();
    closeCreateModal();
}

function onCreateTemplate() {
    var cfg = gatherConfig();
    var nameErr = validateVmName(cfg.name);
    if (nameErr) { sendCmd('log', { message: nameErr }); return; }
    if (cfg.adminPass !== cfg.adminConfirm) {
        sendCmd('log', { message: 'Passwords do not match.' });
        return;
    }
    cfg.isTemplate = true;
    setPending('create:vm:' + cfg.name, { label: 'Creating', name: cfg.name, kind: cfg.osType + ' template', ttl: 10 * 60000,
        expect: function() { return !!findVm(cfg.name); } });
    sendCmd('createVm', cfg);
    clearCreateForm();
    closeCreateModal();
}

/* ---- Create Sandbox modal ---- */

function openCreateModal() {
    /* Reset to defaults every time the modal opens */
    document.getElementById('vm-name').value = 'MyAppSandbox';
    document.getElementById('image-path').value = '';
    selectTemplate('', templateDefaultLabel());
    document.getElementById('hdd-size').value = 64;
    document.getElementById('gpu-mode').value = '1';
    document.getElementById('net-mode').value = '1';
    document.getElementById('admin-user').value = 'user';
    document.getElementById('admin-pass').value = 'test123';
    document.getElementById('admin-confirm').value = 'test123';
    document.getElementById('test-mode').checked = false;
    document.getElementById('ssh-enabled').checked = false;
    document.getElementById('ssh-deploy-key').checked = false;
    onSshToggle();   /* re-grey "Deploy SSH key" to match the cleared SSH checkbox */
    fillDefaultIdentity('vm-identity');   /* Linux guests report a bare-metal desktop unless cleared */
    document.getElementById('replica-auto').checked = false;
    /* Reset OS type to Windows on each open. Valid on both hosts (a Mac host
       supports Windows via QEMU); the user can switch to macOS on a Mac. */
    document.getElementById('os-type').value = 'Windows';

    /* Smart defaults (RAM/cores) from latest host info */
    if (lastHostInfo) applySmartDefaults(lastHostInfo);

    /* Clear validation state */
    document.getElementById('vm-name-warn').textContent = '';
    document.getElementById('admin-user-warn').textContent = '';
    document.getElementById('admin-pass-warn').textContent = '';
    checkPasswordMatch();
    onNetModeChange();
    applyOsTypeUI();   /* fires updateCreateButtons + revalidateVmName */

    document.getElementById('create-vm-overlay').classList.add('active');
    setTimeout(function() { document.getElementById('vm-name').focus(); }, 0);
}

function closeCreateModal() {
    document.getElementById('create-vm-overlay').classList.remove('active');
}

/* Close on backdrop click — but only when the press also STARTED on the backdrop.
   A click targets the common ancestor of the mousedown and mouseup, so pressing
   inside the modal (e.g. selecting text in a field) and releasing on the backdrop
   would otherwise close it. */
let createBackdropPress = false;
document.getElementById('create-vm-overlay').addEventListener('mousedown', function(e) {
    createBackdropPress = (e.target === this);
});
document.getElementById('create-vm-overlay').addEventListener('click', function(e) {
    if (e.target === this && createBackdropPress) closeCreateModal();
    createBackdropPress = false;
});

/* Close on Escape */
document.addEventListener('keydown', function(e) {
    if (e.key !== 'Escape') return;
    if (document.getElementById('create-vm-overlay').classList.contains('active')) {
        closeCreateModal();
    }
    if (document.getElementById('add-overlay').classList.contains('active')) {
        closeAddModal();
    }
});

/* ---- VM Table ---- */

/* Update the status <td> in place. Preserves the spinner element across
   updates so its CSS animation doesn't restart on every staging-file tick.
   The cell carries the lamp (through its class), the state word, a spinner
   while something is in flight and, on a running VM, the agent's small dot. */
function updateStatusCell(td, vm) {
    var needsSpinner = false, label = '', className = '', agent = null, title = '';
    var p = pendingFor('vm:' + vm.name);

    if (p && p.failed) {
        label = p.label + ' failed ';
        className = 'status-failed';
        title = p.failed;
    } else if (p) {
        needsSpinner = true;
        label = p.label + '… ';
        className = 'status-building';
        title = 'Waiting for the host (' + elapsedText(Date.now() - p.since) + ')';
    } else if (vm.buildingVhdx) {
        needsSpinner = true;
        /* vhdxStep names the current phase ("Downloading packages 42/182",
           "Building rootfs", ...); the prefetch downloads used to sit at
           "Building Disk (0%)" for minutes and look hung. */
        if (vm.vhdxStaging)      label = 'Staging files… ';
        else if (vm.vhdxStep)    label = vm.vhdxStep + ' (' + (vm.vhdxProgress || 0) + '%) ';
        else                     label = 'Building disk (' + (vm.vhdxProgress || 0) + '%) ';
        className = 'status-building';
    } else if (vm.running && vm.shuttingDown) {
        needsSpinner = true;
        className = 'status-shutting-down';
        label = 'Shutting down… ';
    } else if (vm.running && vm.isTemplate) {
        needsSpinner = true;
        label = 'Building template ';
        className = 'status-building';
    } else if (vm.running && !vm.installComplete && !vm.isTemplate) {
        needsSpinner = true;
        var defaultLabel = vm.osType === 'macOS' ? 'Installing macOS ' : vm.osType === 'Linux' ? 'Installing Linux ' : 'Installing Windows ';
        label = (vm.installStatus && vm.installStatus.length > 0) ? (vm.installStatus + ' ') : defaultLabel;
        className = 'status-building';
    } else if (vm.running) {
        className = 'status-running';
        label = vm.isHost ? 'Up' : 'Running';
    } else {
        className = 'status-stopped';
        label = 'Stopped';
    }
    /* the in-VM agent is what lets the host manage the guest (and reach its
       replicas and seats); it only means something while the VM runs */
    if (vm.running && !vm.isTemplate && !vm.isHost && !vm.buildingVhdx) {
        agent = { online: !!vm.agentOnline,
                  title: vm.agentOnline ? 'In-VM agent connected: the host can manage the guest, its replicas and seats'
                                        : 'In-VM agent not connected yet: SSH, replicas and seats wait for it' };
    }

    td.className = className;
    td.title = title;
    var existingSpinner = td.querySelector('.spinner');
    var child = td.firstChild;
    while (child) {
        var next = child.nextSibling;
        if (child !== existingSpinner) td.removeChild(child);
        child = next;
    }
    if (needsSpinner) {
        if (!existingSpinner) {
            existingSpinner = document.createElement('span');
            existingSpinner.className = 'spinner';
            td.appendChild(existingSpinner);
        }
        td.insertBefore(document.createTextNode(label), existingSpinner);
    } else {
        if (existingSpinner) td.removeChild(existingSpinner);
        td.appendChild(document.createTextNode(label));
    }
    if (agent) {
        var dot = document.createElement('span');
        dot.className = 'agent-dot' + (agent.online ? ' online' : '');
        dot.title = agent.title;
        td.appendChild(dot);
    }
}

/* ---- Actions: one cell per row, the buttons in groups ----
     [start · display · ssh] [add · screens · identity] [shut down · force stop] [edit · delete]
   The groups keep their order on every row, so the eye finds a button in the
   same place; what a row cannot do is simply not there. */
function actBtn(cls, glyph, active, handler, title, extra) {
    var btn = document.createElement('button');
    btn.className = 'icon-btn ' + cls + (active ? '' : ' inactive') + (extra ? ' ' + extra : '');
    btn.innerHTML = iconMarkup(cls, glyph);
    if (title) btn.title = title;
    if (active) btn.onclick = function(e) { e.stopPropagation(); handler(); };
    else btn.disabled = true;
    return btn;
}
function actionsCell(groups) {
    var td = document.createElement('td');
    td.className = 'actions-col';
    var wrap = document.createElement('div');
    wrap.className = 'actions';
    var first = true;
    groups.forEach(function(g) {
        g = g || [];
        if (!g.length) return;
        if (!first) { var sep = document.createElement('i'); sep.className = 'act-sep'; wrap.appendChild(sep); }
        first = false;
        var span = document.createElement('span');
        span.className = 'act-group';
        /* a slot stays a slot when its button is not there, so the same
           button sits at the same x on every row of the same kind */
        g.forEach(function(b) {
            if (b) { span.appendChild(b); return; }
            var slot = document.createElement('span');
            slot.className = 'act-slot';
            span.appendChild(slot);
        });
        wrap.appendChild(span);
    });
    td.appendChild(wrap);
    return td;
}

/* a VM-level wait: the status cell shows `label…` until expect() holds */
function pendVm(name, label, ttl, expect) {
    setPending('vm:' + name, { label: label, name: name, ttl: ttl, expect: expect });
}

/* Build the list of <td> cells for a row. The status cell is passed in and
   updated in place (rather than recreated) so the spinner animation survives. */
function buildRowCells(vm, i, statusTd) {
    updateStatusCell(statusTd, vm);

    var bld = vm.buildingVhdx;
    var busy = !!pendingFor('vm:' + vm.name);       /* a command is in flight: no second one */
    var snapVal = selectedSnap[i] || 'current';
    var reps = parseReplicas(vm.replicas);
    var live = reps.filter(function(r) { return r.state === 'running' && r.vnc; });
    var tiles = live.map(function(r) { return r.name + ':' + r.vnc + (r.kind === 'seat' ? ':seat' : ''); }).join(',');
    var isLinux = vm.osType === 'Linux';
    var rep = vm.replica || '';

    /* -- open: ssh -- */
    var sshBtn = null;
    if (vm.sshEnabled) {
        var sshActive = (vm.sshState === 2 || vm.sshState === 4) && vm.running && !bld;
        var sshTitle = vm.sshState === 1 ? 'Installing OpenSSH in the guest…'
                     : vm.sshState === 4 ? 'Open an SSH terminal (localhost:' + vm.sshPort + '; the Nestbox key is deployed, key auth works)'
                     : vm.sshState === 2 ? 'Open an SSH terminal to the VM (localhost:' + vm.sshPort + ', tunneled over HvSocket)'
                     : vm.sshState === 3 ? 'SSH install failed'
                     : 'SSH: waiting for the in-VM agent';
        sshBtn = actBtn('ssh', '>_', sshActive, function() { sendCmd('sshConnect', {vmIndex: i}); }, sshTitle);
    }

    /* -- nested: add a replica or a seat, every running screen tiled, the
          legacy single replica / a foreign VNC server, the identity -- */
    var addBtn = null, screensBtn = null, legacyBtn = null;
    if (isLinux && vm.running && vm.agentOnline && !bld && (reps.length || !vm.vncPort)) {
        addBtn = actBtn('add', '+', true, function() { openAddModal(i); },
            reps.length ? 'Add a nested replica or a Steam seat to ' + vm.name
                        : 'Add a nested replica (a KVM guest with its own machine identity) or a Steam seat (a user with an XFCE + Steam desktop) to ' + vm.name);
    } else if (rep === 'stopped' && vm.running && !bld) {
        legacyBtn = actBtn('vnc', '▶️', vm.agentOnline && !busy,
            function() { sendCmd('replicaStart', {vmIndex: i}); }, 'Start the nested replica');
    } else if (rep === 'running' && !vm.vncPort && vm.running && !bld) {
        legacyBtn = actBtn('vnc', 'spinner', false, null, 'Nested replica is starting: waiting for its console');
    } else if (vm.vncPort && vm.running && !bld) {
        legacyBtn = actBtn('vnc', '🖥️', true, function() { sendCmd('vncOpen', {vmIndex: i}); },
            rep === 'running' ? 'Open the nested replica\'s screen in its own window'
                              : 'Open the guest\'s VNC server (port ' + vm.vncPort + ') in its own window');
    }
    if (live.length)
        screensBtn = actBtn('vnc', 'grid', true, function() { sendCmd('vncGrid', {vmIndex: i, tiles: tiles}); },
            live.length === 1 ? 'Open the running screen in a grid window (more tiles appear as replicas and seats start)'
                              : 'Open all ' + live.length + ' running screens side by side in one window');
    var identPending = pendingFor('ident:' + vm.name);
    var identBtn = isLinux ? actBtn('identity', identPending ? 'spinner' : 'id', !bld && !identPending, function() {
            setPending('ident:' + vm.name, { label: 'identity', ttl: 10000 });
            sendCmd('getIdentity', {vmIndex: i});
        }, 'VM identity: what the guest reports about its machine (DMI strings, systemd-detect-virt, chipset; ACPI / SMBIOS / drive / CPUID strings for the replicas)') : null;

    /* This PC (Linux host): the machine itself. Nothing to start or stop:
       the "+" for replicas and seats, their screens, and the identity
       profile they are built from. */
    if (vm.isHost) {
        var nameTd = makeCell(vm.name, i, 0, 'This PC: the replicas and seats below run on it directly');
        var tag = document.createElement('span');
        tag.className = 'chip';
        tag.textContent = 'this PC';
        nameTd.appendChild(tag);
        /* The identity-patched QEMU (hypervisor-level identity strings for
           the replicas): built once on this PC, from here. */
        var qemu = document.createElement('span');
        qemu.className = 'hint mono qemu-state';
        if (vm.qemuPatched) {
            qemu.textContent = 'qemu: identity-patched ✔';
            qemu.title = 'The identity-patched QEMU is installed: ACPI / SMBIOS / drive / CPUID strings from the profile reach the replicas';
        } else if (vm.qemuBuilding) {
            qemu.innerHTML = 'qemu: building the patch <span class="spinner"></span>';
            qemu.title = 'appsandbox-replica qemu build is running (~10 min); progress is in the log';
        } else {
            qemu.textContent = 'qemu: stock ';
            var qb = document.createElement('button');
            qb.className = 'mini';
            qb.textContent = 'Build patch';
            qb.title = 'Build QEMU 8.2.2 with the identity patches and install it over the distro binary (~10 min, once). ' +
                       'Without it the replicas run on the stock QEMU: DMI strings and the hidden hypervisor flag still work, ' +
                       'the ACPI / SMBIOS-manufacturer / drive / CPUID strings are ignored.';
            qb.onclick = function(e) {
                e.stopPropagation();
                showModal('Identity-patched QEMU',
                    'Builds QEMU 8.2.2 with the identity patches on this PC and installs it over the distro binary ' +
                    '(dpkg-divert, reversible with "appsandbox-replica qemu restore"). Takes about 10 minutes and ' +
                    'downloads build dependencies plus the QEMU source. Running replicas keep the stock QEMU until their next boot.',
                    'Build', { confirmClass: 'primary' })
                .then(function(ok) { if (ok) sendCmd('qemuBuild', {}); });
            };
            qemu.appendChild(qb);
        }
        nameTd.appendChild(qemu);
        if (vm.kvm === false) {
            var nokvm = document.createElement('span');
            nokvm.className = 'chip warn';
            nokvm.textContent = 'no /dev/kvm';
            nokvm.title = 'KVM is not available: enable virtualization (VT-x / AMD-V) in the firmware; replicas cannot run without it (seats can)';
            nameTd.appendChild(nokvm);
        }
        var hostCells = [
            nameTd,
            makeCell(vm.osName || 'Linux', i, 1),
            statusTd,
            makeCell(vm.cpuCores, i, 4, 'CPU cores of this PC'),
            makeCell(vm.ramMb + ' MB', i, 5, 'Memory of this PC'),
            makeCell(vm.hddGb + ' GB', i, 6, 'Size of the root filesystem'),
            makeCell(vm.gpuName || 'host GPU', i, 7, 'GPU of this PC'),
            makeCell('host', i, 8, 'The replicas use libvirt\'s NAT network (virbr0); a seat uses this PC\'s own network'),
        ];
        if (!hostBridge.noSnapshots) hostCells.push(makeCell('', i, 9));
        hostCells.push(actionsCell([[addBtn || legacyBtn, screensBtn, identBtn]]));
        return hostCells;
    }

    var startBtn = actBtn('start', '▶️', !vm.running && !bld && !busy, (function(vmIdx, sv, vmObj) { return function() {
        var p = parseSnapValue(sv);
        var go = function(extra) {
            pendVm(vmObj.name, 'Starting', 120000, function() { var v = findVm(vmObj.name); return !v || v.running; });
            sendCmd('startVm', Object.assign({ vmIndex: vmIdx, snapIndex: p.snapIndex, branchIndex: p.branchIndex }, extra || {}));
        };
        if ((p.snapIndex >= 0 || p.snapIndex === -2) && p.branchIndex < 0) {
            /* Creating a new branch: prompt for its name */
            var parentName = p.snapIndex === -2 ? 'Base' : ((vmObj.snapshots && vmObj.snapshots[p.snapIndex]) ? vmObj.snapshots[p.snapIndex].name : 'Snapshot');
            var now = new Date();
            var pad = function(n) { return n < 10 ? '0' + n : '' + n; };
            var defaultName = now.getFullYear() + '-' + pad(now.getMonth()+1) + '-' + pad(now.getDate()) + ' ' + pad(now.getHours()) + ':' + pad(now.getMinutes()) + ':' + pad(now.getSeconds());
            showModal('New branch', 'A new branch will be created from ' + parentName + '. Branches are independent working copies: changes in one branch do not affect the others or the base snapshot.', 'Boot', {
                confirmClass: 'primary',
                input: { label: 'Branch name:', value: defaultName }
            }).then(function(result) {
                if (result === false) return;
                selectedSnap[vmIdx] = 'current';
                go({ branchName: result });
            });
        } else {
            go();
        }
    }; })(i, snapVal, vm), 'Start the VM (boots from the selected snapshot / branch)');
    var displayBtn = actBtn('connect-idd', '📺', vm.running && !bld, function() { sendCmd('connectIddVm', {vmIndex: i}); }, 'Open the VM display window (IDD virtual monitor)');
    var shutdownBtn = actBtn('shutdown', '⏻', vm.running && !bld && !busy && !vm.shuttingDown, function() {
        pendVm(vm.name, 'Shutting down', 180000, function() { var v = findVm(vm.name); return !v || !v.running || v.shuttingDown; });
        sendCmd('shutdownVm', {vmIndex: i});
    }, 'Ask the guest OS to shut down');
    var stopBtn = actBtn('stop', '✕️', vm.running && !bld && !busy, function() { onStopVm(i); }, 'Force power off the VM at once (may lose unsaved guest data)');
    var deleteBtn = actBtn('delete', '🗑️', !bld && !busy, function() { onDeleteVm(i); }, 'Delete this VM and its virtual disks', vm.running ? 'running' : '');
    var editBtn = actBtn('edit', editModeRow === i ? '✔️' : '✏️', !vm.running && !bld && !busy, function() { toggleEditMode(i); },
        editModeRow === i ? 'Done editing' : 'Edit CPU, RAM, GPU and network (the VM must be stopped)');

    var cells = [
        makeCell(vm.name, i, 0),
        makeCell(vm.osType, i, 1),
        statusTd,
        makeCell(vm.cpuCores, i, 4, 'Virtual CPU cores of this VM'),
        makeCell(vm.ramMb + ' MB', i, 5, 'Memory reserved for this VM'),
        makeCell(vm.hddGb + ' GB', i, 6, 'Virtual disk size'),
        makeCell(vm.gpuName || (vm.gpuMode === 2 ? 'Try all' : vm.gpuMode === 1 ? 'Default GPU' : 'None'), i, 7, 'GPU passed through to the VM via GPU-PV, or None'),
        makeCell(netNames[vm.networkMode] || 'None', i, 8, 'Networking: NAT (shared), External (bridged), Internal (host-only), or None'),
    ];
    if (!hostBridge.noSnapshots) cells.push(makeSnapCell(vm, i));
    cells.push(actionsCell([[startBtn, displayBtn, sshBtn], [addBtn || legacyBtn, screensBtn, identBtn], [shutdownBtn, stopBtn], [editBtn, deleteBtn]]));
    return cells;
}

/* the number of columns before Actions: colSpan of a replica / seat row */
function dataColCount() { return 8 + (hostBridge.noSnapshots ? 0 : 1); }

function renderVmTable() {
    var tbody = document.getElementById('vm-table');   /* one <tbody class="vm-group"> per VM (its row + replica rows) */
    resolvePending();

    /* the skeleton rows stand in until the host has said what exists */
    var sk = document.getElementById('vm-skeleton');
    if (!firstStateSeen) return;
    if (sk && sk.parentNode) sk.parentNode.removeChild(sk);

    var vc = document.getElementById('vm-count');
    if (vc) {
        var nrep = 0, nseat = 0;
        vms.forEach(function(vm) { parseReplicas(vm.replicas).forEach(function(r) { if (r.kind === 'seat') nseat++; else nrep++; }); });
        var parts = [];
        if (hostBridge.isLinux) parts.push('this PC');
        else if (vms.length) parts.push(vms.length + (vms.length === 1 ? ' sandbox' : ' sandboxes'));
        if (nrep) parts.push(nrep + (nrep === 1 ? ' replica' : ' replicas'));
        if (nseat) parts.push(nseat + (nseat === 1 ? ' seat' : ' seats'));
        vc.textContent = parts.join(' · ');
    }

    /* sandboxes the host has not listed yet: a row each until it does */
    var creating = Object.keys(pending).filter(function(k) {
        return k.indexOf('create:vm:') === 0 && !findVm(k.slice(10));
    });

    if (vms.length === 0 && !creating.length) {
        rowCache = {};
        rowSigCache = {};
        Array.prototype.slice.call(tbody.children).forEach(function(c) { if (c.tagName !== 'THEAD') tbody.removeChild(c); });
        var grp0 = document.createElement('tbody');
        var tr = document.createElement('tr');
        var td = document.createElement('td');
        td.colSpan = dataColCount() + 1;
        td.className = 'empty-state';
        td.innerHTML =
            '<div class="empty">' +
              '<svg class="ic empty-mark"><use href="#i-nestbox"/></svg>' +
              '<div class="empty-title">No sandboxes yet</div>' +
              '<div class="empty-text">A sandbox is a Windows or Linux VM on this PC. A Linux sandbox can hold nested replicas (KVM guests with their own machine identity) and Steam seats (extra Steam desktops, no VM).</div>' +
            '</div>';
        var btn = document.createElement('button');
        btn.className = 'primary empty-state-btn';
        btn.innerHTML = '<svg class="ic"><use href="#i-plus"/></svg>New sandbox';
        btn.onclick = openCreateModal;
        td.firstChild.appendChild(btn);
        tr.appendChild(td);
        grp0.appendChild(tr);
        tbody.appendChild(grp0);
        return;
    }

    /* Drop cached rows for VMs that no longer exist. */
    var seen = {};
    vms.forEach(function(vm) { seen[vm.name] = true; });
    Object.keys(rowCache).forEach(function(name) {
        if (!seen[name]) {
            var stale = rowCache[name];
            if (stale.parentNode) stale.parentNode.removeChild(stale);
            delete rowCache[name];
            delete rowSigCache[name];
        }
    });

    /* Remove any non-cached tbody children (the empty-state row, the
       placeholder groups of the previous render). */
    var kids = Array.prototype.slice.call(tbody.children);
    kids.forEach(function(c) {
        if (c.tagName === 'THEAD') return;
        var cached = false;
        for (var n in rowCache) { if (rowCache[n] === c) { cached = true; break; } }
        if (!cached) tbody.removeChild(c);
    });

    /* Skip the cell rebuild when button-relevant fields are unchanged; the
     * install progress tick would otherwise destroy the button DOM mid-click. */
    vms.forEach(function(vm, i) {
        var grp = rowCache[vm.name];
        var firstBuild = !grp;
        if (!grp) {
            grp = document.createElement('tbody');
            grp.className = 'vm-group';
            grp.appendChild(document.createElement('tr'));
            rowCache[vm.name] = grp;
        }
        var tr = grp.firstChild;

        var statusTd = tr.children[2] || document.createElement('td');
        updateStatusCell(statusTd, vm);

        var sig = [
            i === selectedVm, editModeRow === i,
            vm.running, vm.buildingVhdx, vm.shuttingDown, vm.agentOnline,
            vm.installComplete, vm.isTemplate,
            vm.sshEnabled, vm.sshState, vm.sshPort,
            vm.vncPort, vm.replica, vm.replicas, vm.hasIdentity,   /* nested / VNC / identity cells */
            vm.osType, vm.ramMb, vm.hddGb, vm.cpuCores, vm.isHost, vm.osName, vm.qemuPatched, vm.qemuBuilding, vm.kvm,
            vm.gpuMode, vm.gpuName, vm.networkMode,
            selectedSnap[i] || 'current',
            pendingSig(vm.name),                                     /* what the page waits on for this VM */
            /* Snapshot tree: take/delete/rename/branch must trigger a row rebuild
               so makeSnapCell re-runs. These fields only change on user snapshot
               actions (never on install-progress ticks — a VM can't be snapshotted
               while running), so the rebuild-skip optimization above is preserved. */
            vm.hasSnapshots, vm.snapCurrent, vm.snapCurrentBranch,
            JSON.stringify(vm.snapshots || []), JSON.stringify(vm.baseBranches || [])
        ].join('|');

        if (!firstBuild && rowSigCache[vm.name] === sig) {
            if (tbody.children[i + 1] !== grp) {            /* children[0] is the <thead> */
                tbody.insertBefore(grp, tbody.children[i + 1] || null);
            }
            return;
        }
        rowSigCache[vm.name] = sig;

        tr.className = (i === selectedVm ? 'selected ' : '') +
                       (vm.running ? 'running' : 'stopped');
        tr.onclick = function(e) {
            if (e.target.closest('.icon-btn')) return;
            if (e.target.closest('.editing')) return;
            if (e.target.closest('.snap-cell')) return;
            if (e.target.closest('button')) return;
            selectVm(i);
        };

        var cells = buildRowCells(vm, i, statusTd);

        for (var c = 0; c < cells.length; c++) {
            var newCell = cells[c];
            var oldCell = tr.children[c];
            if (oldCell === newCell) continue;
            if (oldCell) tr.replaceChild(newCell, oldCell);
            else tr.appendChild(newCell);
        }
        while (tr.children.length > cells.length) tr.removeChild(tr.lastChild);

        buildReplicaRows(grp, vm, i);

        if (tbody.children[i + 1] !== grp) {
            tbody.insertBefore(grp, tbody.children[i + 1] || null);
        }
    });

    creating.forEach(function(k) {
        var p = pending[k], name = k.slice(10);
        var grp = document.createElement('tbody');
        grp.className = 'pending-group';
        var tr = document.createElement('tr');
        tr.className = 'pending-row';
        var nameTd = document.createElement('td');
        nameTd.textContent = name;
        var osTd = document.createElement('td');
        osTd.textContent = p.kind || '';
        var st = document.createElement('td');
        if (p.failed) {
            st.className = 'status-failed';
            st.textContent = 'Creating failed';
            st.title = p.failed;
        } else {
            st.className = 'status-building';
            st.innerHTML = 'Creating… <span class="spinner"></span>';
        }
        var rest = document.createElement('td');
        rest.colSpan = dataColCount() - 3;
        rest.className = 'hint';
        rest.innerHTML = p.failed ? 'See the log below.' : 'Waiting for the host to list it <span class="mono" data-pending-elapsed="' + k + '">' + elapsedText(Date.now() - p.since) + '</span>';
        var dismiss = actBtn('dismiss', 'x', true, function() { clearPending(k); }, 'Dismiss');
        tr.appendChild(nameTd); tr.appendChild(osTd); tr.appendChild(st); tr.appendChild(rest);
        tr.appendChild(actionsCell([[dismiss]]));
        grp.appendChild(tr);
        tbody.appendChild(grp);
    });
}

/* ---- Nested replicas and Steam seats: rows under their VM ----
   vm.replicas is the agent's JSON list [{name, state, vnc, desktop[, kind,
   res, cpus, ram, disk]}]: the nested replicas (KVM guests) and, with kind
   "seat", the Steam seats (a Linux user with an Xvnc display + XFCE + Steam
   on the machine itself). They are not VMs, so their rows do not borrow the
   VM's columns: one cell with the name, the kind, the state and a spec line,
   then the row's own actions. */
function parseReplicas(str) {
    if (!str) return [];
    try { var v = JSON.parse(str); return Array.isArray(v) ? v : []; } catch (e) { return []; }
}

function pendRep(vmName, name, label, ttl, expect) {
    setPending('rep:' + vmName + '/' + name, { label: label, name: name, ttl: ttl, expect: expect, doneOnLog: true });
}

function buildReplicaRows(grp, vm, idx) {
    while (grp.children.length > 1) grp.removeChild(grp.lastChild);
    if (vm.osType !== 'Linux' || !vm.running || !vm.agentOnline || vm.buildingVhdx) return;
    var reps = parseReplicas(vm.replicas);
    var cols = dataColCount();
    reps.forEach(function(r) { grp.appendChild(replicaRow(vm, idx, r, cols)); });
    /* what is being created: a row of its own until the host lists it */
    var prefix = 'create:rep:' + vm.name + '/';
    var placeholders = 0;
    Object.keys(pending).forEach(function(k) {
        if (k.indexOf(prefix) !== 0) return;
        var name = k.slice(prefix.length);
        if (reps.some(function(r) { return r.name === name; })) return;
        grp.appendChild(pendingRepRow(vm, k, name, pending[k], cols));
        placeholders++;
    });
    /* this PC with nothing on it yet: say what the "+" is for */
    if (vm.isHost && !reps.length && !placeholders) {
        var tr = document.createElement('tr');
        tr.className = 'replica-row hint-row';
        var td = document.createElement('td');
        td.colSpan = cols + 1;
        td.className = 'replica-cell';
        td.innerHTML = '<span class="replica-arm"></span><span class="hint">Nothing here yet. The <b>+</b> adds a nested replica or a Steam seat to this PC.</span>';
        tr.appendChild(td);
        grp.appendChild(tr);
    }
}

function replicaRow(vm, idx, r, cols) {
    var seat = r.kind === 'seat';
    var act = seat ? 'seat' : 'replica';   /* action prefix: seatStart / replicaStart ... */
    var running = r.state === 'running';
    var name = r.name;
    var key = 'rep:' + vm.name + '/' + name;
    var p = pendingFor(key);
    var busy = !!p;

    var tr = document.createElement('tr');
    tr.className = 'replica-row ' + (running ? 'running' : 'stopped') + (seat ? ' seat' : '');
    var td = document.createElement('td');
    td.colSpan = cols;
    td.className = 'replica-cell';
    td.innerHTML = '<span class="replica-arm"></span><svg class="ic"><use href="#' + (seat ? 'i-user' : 'i-nest') + '"/></svg>' +
                   '<span class="replica-name"></span><span class="chip kind"></span>' +
                   '<span class="replica-state"></span><span class="replica-spec mono"></span>';
    td.querySelector('.replica-name').textContent = name;
    td.querySelector('.chip').textContent = seat ? 'seat' : 'replica';
    td.title = seat
        ? 'Steam seat: user "' + name + '" (password test123) with its own Xvnc display, XFCE and Steam on ' + vm.name + ' itself (appsandbox-seat). ' +
          'No VM: it shows the machine\'s identity (DMI, machine-id, disks, MAC) and uses only the memory its programs take.'
        : 'Nested replica: a KVM guest inside ' + vm.name + ' (appsandbox-replica) with the identity profile\'s machine identity.';

    var stEl = td.querySelector('.replica-state');
    if (p && p.failed) {
        stEl.className = 'replica-state failed';
        stEl.innerHTML = '<span class="lamp err"></span>';
        stEl.appendChild(document.createTextNode(p.label + ' failed'));
        stEl.title = p.failed;
    } else if (p) {
        stEl.className = 'replica-state busy';
        stEl.innerHTML = '<span class="lamp warn"></span>';
        stEl.appendChild(document.createTextNode(p.label + '…'));
        var sp = document.createElement('span'); sp.className = 'spinner'; stEl.appendChild(sp);
    } else {
        stEl.className = 'replica-state ' + (running ? 'run' : 'off');
        stEl.innerHTML = '<span class="lamp' + (running ? ' run' : '') + '"></span>';
        stEl.appendChild(document.createTextNode(running ? 'running' : (r.state || 'stopped')));
    }

    var spec = seat
        ? ['xfce', r.res || '', r.steamApp ? 'steam app ' + r.steamApp : 'steam', r.vnc ? 'vnc :' + r.vnc : '']
        : [r.cpus ? r.cpus + ' cores' : '', r.ram ? r.ram + ' MB' : '', r.disk ? r.disk + ' GB' : '',
           r.desktop ? 'xfce' : 'no desktop', r.vnc ? 'vnc :' + r.vnc : ''];
    var specEl = td.querySelector('.replica-spec');
    specEl.textContent = spec.filter(Boolean).join('  ·  ');
    specEl.title = seat
        ? 'An XFCE desktop on an Xvnc display' + (r.res ? ' of ' + r.res : '') + ', Steam at login' + (r.steamApp ? ' opening app ' + r.steamApp : '') +
          (r.vnc ? '; the display listens on 127.0.0.1:' + r.vnc + ' inside ' + vm.name : '') + '. Shares ' + vm.name + '\'s cores, memory, disk and network.'
        : 'Cores, memory and disk reserved for the replica (the disk grows only)' + (r.desktop ? ', XFCE + Steam installed' : ', no desktop yet') +
          (r.vnc ? '; its console listens on 127.0.0.1:' + r.vnc + ' inside ' + vm.name : '') + '. Network: libvirt NAT (virbr0).';
    tr.appendChild(td);

    var startBtn = actBtn('start', '▶️', !running && !busy, function() {
        pendRep(vm.name, name, 'Starting', 90000, function() { var x = findRep(vm.name, name); return !x || x.state === 'running'; });
        sendCmd(act + 'Start', {vmIndex: idx, name: name});
    }, 'Start ' + act + ' "' + name + '"');
    var screenBtn = actBtn('connect-idd', '🖥️', running && !!r.vnc,
        function() { sendCmd('vncOpen', {vmIndex: idx, port: r.vnc, name: name, kind: act}); }, 'Open the screen of ' + act + ' "' + name + '" in its own window');
    var sizeBtn = seat ? null : actBtn('edit', 'edit', !busy, function() { resizeReplica(idx, r); }, 'Cores, RAM and disk of replica "' + name + '"');
    var desktopBtn = (seat || r.desktop) ? null : actBtn('vnc', 'desktop', running && !busy, function() {
        confirmReplica(idx, name, 'replicaDesktop', 'Install XFCE + Steam in "' + name + '"? Takes 10-20 minutes and restarts the replica.', 'Install', function() {
            pendRep(vm.name, name, 'Installing the desktop', 40 * 60000, function() { var x = findRep(vm.name, name); return !x || !!x.desktop; });
        });
    }, 'Install XFCE + Steam (autologin) in this replica');
    var stopBtn = actBtn('shutdown', '⏻', running && !busy, function() {
        pendRep(vm.name, name, 'Stopping', 90000, function() { var x = findRep(vm.name, name); return !x || x.state !== 'running'; });
        sendCmd(act + 'Stop', {vmIndex: idx, name: name});
    }, seat ? 'Stop this seat: its XFCE session and Steam end (the user account stays)' : 'Shut this replica down');
    var restartBtn = actBtn('restart', '↻', running && !busy, function() {
        pendRep(vm.name, name, 'Restarting', seat ? 60000 : 120000, null);
        sendCmd(act + 'Restart', {vmIndex: idx, name: name});
    }, seat ? 'Restart this seat (a fresh XFCE session)' : 'Restart this replica (picks up a changed identity)');
    var deleteBtn = actBtn('delete', '🗑️', !busy, function() {
        confirmReplica(idx, name, act + 'Destroy',
            seat ? 'Delete seat "' + name + '"? Its user account and home directory (/home/' + name + ', with Steam\'s files) are removed. This cannot be undone.'
                 : 'Delete replica "' + name + '" and its disk? This cannot be undone.', 'Delete', function() {
            pendRep(vm.name, name, 'Deleting', 180000, function() { return !findRep(vm.name, name); });
        });
    }, 'Delete this ' + act, running ? 'running' : '');
    tr.appendChild(actionsCell([[startBtn, screenBtn], [sizeBtn, desktopBtn], [stopBtn, restartBtn], [deleteBtn]]));
    return tr;
}

/* a replica or seat that was asked for and is not listed yet */
function pendingRepRow(vm, key, name, p, cols) {
    var seat = p.kind === 'seat';
    var tr = document.createElement('tr');
    tr.className = 'replica-row pending-row' + (seat ? ' seat' : '');
    var td = document.createElement('td');
    td.colSpan = cols;
    td.className = 'replica-cell';
    td.innerHTML = '<span class="replica-arm"></span><svg class="ic"><use href="#' + (seat ? 'i-user' : 'i-nest') + '"/></svg>' +
                   '<span class="replica-name"></span><span class="chip kind"></span><span class="replica-state"></span><span class="replica-spec"></span>';
    td.querySelector('.replica-name').textContent = name;
    td.querySelector('.chip').textContent = seat ? 'seat' : 'replica';
    var stEl = td.querySelector('.replica-state'), specEl = td.querySelector('.replica-spec');
    if (p.failed) {
        stEl.className = 'replica-state failed';
        stEl.innerHTML = '<span class="lamp err"></span>creating failed';
        stEl.title = p.failed;
        specEl.className = 'replica-spec hint';
        specEl.textContent = 'See the log below.';
    } else {
        stEl.className = 'replica-state busy';
        stEl.innerHTML = '<span class="lamp warn"></span>creating… <span class="spinner"></span>';
        specEl.className = 'replica-spec hint';
        specEl.innerHTML = (seat ? 'packages the first time (~1.5 GB), then seconds; the row appears when the seat is up'
                                 : 'cloud image, then XFCE + Steam (10–20 min); the row appears when it boots') +
                           ' · <span class="mono" data-pending-elapsed="' + key + '">' + elapsedText(Date.now() - p.since) + '</span>';
    }
    tr.appendChild(td);
    var dismiss = actBtn('dismiss', 'x', true, function() { clearPending(key); }, p.failed ? 'Dismiss' : 'Stop waiting (the creation itself goes on; the row appears when the host lists it)');
    tr.appendChild(actionsCell([[dismiss]]));
    return tr;
}

function confirmReplica(idx, name, action, message, label, onOk) {
    var seat = action.indexOf('seat') === 0;
    showModal(seat ? 'Steam seat' : 'Nested replica', message, label, { confirmClass: /Destroy$/.test(action) ? 'danger' : 'primary' }).then(function(ok) {
        if (!ok) return;
        if (onOk) onOk();
        sendCmd(action, {vmIndex: idx, name: name});
    });
}

/* What a replica may be given: the sandbox's cores, and its RAM minus what
   the sandbox itself needs to stay comfortable. */
function replicaLimits(vm) {
    var cores = (vm && vm.cpuCores) || 8;
    var ram = (vm && vm.ramMb) || 8192;
    return { cores: cores, ram: Math.max(1024, ram - 2048), vmRam: ram };
}

/* ---- "+" on a sandbox (or this PC): the Add dialog ----
   A nested replica and a Steam seat are different things, so the dialog
   asks which first and shows only that kind's fields: a replica is sized
   (cores, RAM, disk), a seat is not - it is a user with a display. */
var addVmIndex = -1;

function nextFreeName(base, taken, n) {
    if (taken.indexOf(base) < 0) return base;
    for (var i = Math.max(2, n + 1); ; i++) if (taken.indexOf(base + i) < 0) return base + i;
}

function addTakenNames(vm) {
    var taken = parseReplicas(vm.replicas).map(function(r) { return r.name; });
    var prefix = 'create:rep:' + vm.name + '/';
    Object.keys(pending).forEach(function(k) { if (k.indexOf(prefix) === 0) taken.push(k.slice(prefix.length)); });
    return taken;
}

function openAddModal(idx) {
    var vm = vms[idx];
    if (!vm) return;
    addVmIndex = idx;
    var taken = addTakenNames(vm);
    var all = parseReplicas(vm.replicas);
    var nrep = all.filter(function(r) { return r.kind !== 'seat'; }).length;
    var nseat = all.length - nrep;
    var lim = replicaLimits(vm);
    var onHost = !!vm.isHost;
    var where = onHost ? 'this PC' : vm.name;
    var g = function(id) { return document.getElementById(id); };

    g('add-eyebrow').textContent = onHost ? 'add to this pc' : 'add to sandbox';
    g('add-title').textContent = 'Add to ' + vm.name;
    g('add-seat-meta').textContent = 'seconds · uses only what its programs take · shows ' + where + '\'s identity';
    g('add-replica-meta').textContent = (onHost ? '10–20 min' : '10–30 min') + ' · reserves cores, RAM and disk · looks like another PC';

    /* replica */
    g('add-rep-name').value = nextFreeName('replica', taken, nrep);
    g('add-rep-cpus').value = Math.min(4, lim.cores);
    g('add-rep-cpus').max = lim.cores;
    g('add-rep-cpus-info').textContent = where + ' has ' + lim.cores;
    g('add-rep-ram').value = Math.min(4096, lim.ram);
    g('add-rep-ram').max = lim.ram;
    g('add-rep-ram-info').textContent = where + ' has ' + lim.vmRam + ' MB; up to ' + lim.ram + ' MB for a replica';
    g('add-rep-disk').value = 20;
    var patch = onHost && !vm.qemuPatched;
    g('add-rep-patch-label').hidden = !patch;
    g('add-rep-patch-row').hidden = !patch;
    g('add-rep-patch').checked = false;
    g('add-rep-note').textContent = onHost
        ? 'Nestbox creates it from the Ubuntu cloud image and installs XFCE + Steam (10–20 min). Its row appears once it boots; every step is in the log.'
        : 'Nestbox builds the identity-patched QEMU inside ' + vm.name + ' (the first time, ~10 min), creates the replica and installs XFCE + Steam (10–20 min). Its row appears once it boots; every step is in the log.';

    /* seat */
    var last = {};
    try { last = JSON.parse(localStorage.getItem('nestbox.seat') || '{}') || {}; } catch (e) {}
    g('add-seat-name').value = nextFreeName('seat', taken, nseat);
    g('add-seat-res').value = last.res || '1600x900';
    g('add-seat-app').value = last.steamApp || '';
    g('add-seat-copy').checked = last.copyGame !== false;
    g('add-seat-auto').value = last.autostart || '';
    /* the game / copy / autostart words only reach a seat on a Linux host;
       inside a sandbox the guest agent takes the screen size alone */
    document.querySelectorAll('#add-form-seat .seat-host-only').forEach(function(el) { el.hidden = !onHost; });
    g('add-seat-note').textContent = 'Packages once (~1.5 GB), then seconds per seat. The seat signs in to Steam itself; the user\'s password is test123. ' +
        (onHost ? 'A game at login and extra programs can be changed later with "sudo appsandbox-seat -n <name> configure".'
                : 'A game at login and extra programs are set inside ' + vm.name + ' with "sudo appsandbox-seat -n <name> configure".');

    var kind = 'replica';
    try { kind = localStorage.getItem('nestbox.addKind') || 'replica'; } catch (e) {}
    g('add-kind-seat').checked = kind === 'seat';
    g('add-kind-replica').checked = kind !== 'seat';
    applyAddKind();
    g('add-overlay').classList.add('active');
    setTimeout(function() { var el = g(kind === 'seat' ? 'add-seat-name' : 'add-rep-name'); el.focus(); el.select(); }, 30);
}

function applyAddKind() {
    var seat = document.getElementById('add-kind-seat').checked;
    document.getElementById('add-form-replica').hidden = seat;
    document.getElementById('add-form-seat').hidden = !seat;
    document.getElementById('btn-add').textContent = seat ? 'Create seat' : 'Create replica';
    document.querySelectorAll('.kind-card').forEach(function(c) { c.classList.toggle('selected', c.querySelector('input').checked); });
    revalidateAdd();
}

function seatNameError(name, taken) {
    if (!name) return 'A user name is required.';
    if (!/^[a-z][a-z0-9_-]{0,30}$/.test(name)) return 'A Linux user name: lowercase letters, digits, - and _, starting with a letter.';
    if (taken.indexOf(name) >= 0) return 'A replica or seat named "' + name + '" already exists.';
    return null;
}
function replicaNameError(name, taken) {
    if (!name) return 'A name is required.';
    if (!/^[A-Za-z0-9._-]{1,48}$/.test(name)) return 'Letters, digits, - _ and . only.';
    if (taken.indexOf(name) >= 0) return 'A replica or seat named "' + name + '" already exists.';
    return null;
}

function revalidateAdd() {
    var vm = vms[addVmIndex];
    if (!vm) return;
    var taken = addTakenNames(vm);
    var seat = document.getElementById('add-kind-seat').checked;
    var err;
    if (seat) {
        err = seatNameError(document.getElementById('add-seat-name').value.trim(), taken);
        document.getElementById('add-seat-warn').textContent = err || '';
    } else {
        err = replicaNameError(document.getElementById('add-rep-name').value.trim(), taken);
        document.getElementById('add-rep-warn').textContent = err || '';
    }
    document.getElementById('btn-add').disabled = !!err;
}
document.getElementById('add-seat-name').addEventListener('input', revalidateAdd);
document.getElementById('add-rep-name').addEventListener('input', revalidateAdd);

function closeAddModal() {
    document.getElementById('add-overlay').classList.remove('active');
    addVmIndex = -1;
}

function onAddConfirm() {
    var idx = addVmIndex, vm = vms[idx];
    if (!vm) return;
    var taken = addTakenNames(vm);
    var seat = document.getElementById('add-kind-seat').checked;
    try { localStorage.setItem('nestbox.addKind', seat ? 'seat' : 'replica'); } catch (e) {}
    if (seat) {
        var sname = document.getElementById('add-seat-name').value.trim().toLowerCase();
        if (seatNameError(sname, taken)) { revalidateAdd(); return; }
        var res = document.getElementById('add-seat-res').value;
        var app = document.getElementById('add-seat-app').value.replace(/\D/g, '');
        var copy = document.getElementById('add-seat-copy').checked;
        var cmd = document.getElementById('add-seat-auto').value.trim();
        try { localStorage.setItem('nestbox.seat', JSON.stringify({ res: res, steamApp: app, copyGame: copy, autostart: cmd })); } catch (e) {}
        var autostart = [];
        if (cmd) {
            /* the entry's name: the program's basename (the last path-like word) */
            var toks = cmd.split(/\s+/), paths = toks.filter(function(t) { return t.indexOf('/') >= 0; });
            autostart.push((paths.length ? paths[paths.length - 1] : toks[0]).split('/').pop() + '=' + cmd);
        }
        setPending('create:rep:' + vm.name + '/' + sname, { label: 'Creating', name: sname, kind: 'seat', ttl: 20 * 60000,
            expect: function() { return !!findRep(vm.name, sname); } });
        sendCmd('seatCreate', {vmIndex: idx, name: sname, res: res, steam: true, steamApp: app, copyGame: copy, autostart: autostart});
    } else {
        var name = document.getElementById('add-rep-name').value.trim();
        if (replicaNameError(name, taken)) { revalidateAdd(); return; }
        var lim = replicaLimits(vm);
        var cpus = Math.min(lim.cores, Math.max(1, parseInt(document.getElementById('add-rep-cpus').value, 10) || 4));
        var ram = Math.min(lim.ram, Math.max(512, parseInt(document.getElementById('add-rep-ram').value, 10) || 4096));
        var disk = Math.min(2048, Math.max(5, parseInt(document.getElementById('add-rep-disk').value, 10) || 20));
        var patch = !document.getElementById('add-rep-patch-row').hidden && document.getElementById('add-rep-patch').checked;
        setPending('create:rep:' + vm.name + '/' + name, { label: 'Creating', name: name, kind: 'replica', ttl: 60 * 60000,
            expect: function() { return !!findRep(vm.name, name); } });
        sendCmd('replicaSetup', {vmIndex: idx, name: name, cpus: cpus, ram: ram, disk: disk, patch: patch});
    }
    closeAddModal();
}

(function() {
    var press = false;
    var ov = document.getElementById('add-overlay');
    ov.addEventListener('mousedown', function(e) { press = (e.target === this); });
    ov.addEventListener('click', function(e) { if (e.target === this && press) closeAddModal(); press = false; });
    ov.addEventListener('keydown', function(e) {
        if (e.key === 'Enter' && e.target.tagName === 'INPUT' && e.target.type !== 'checkbox' && e.target.type !== 'radio') {
            e.preventDefault();
            if (!document.getElementById('btn-add').disabled) onAddConfirm();
        }
    });
})();

/* Pencil on a replica row: cores, RAM and disk. Cores and RAM are redefined
   in libvirt and apply when the replica next boots (now, with the restart
   box). The disk can only grow; cloud-init's growpart extends the root
   filesystem at the next boot. */
function resizeReplica(idx, r) {
    var vm = vms[idx] || {};
    var lim = replicaLimits(vm);
    var running = r.state === 'running';
    showModal('Replica "' + r.name + '": size',
        'Limited by ' + (vm.isHost ? 'this PC' : 'the sandbox') + ' (' + lim.cores + ' cores, ' + lim.vmRam + ' MB). Cores and RAM apply when the replica next boots; ' +
        'the disk can only grow, and the root filesystem extends itself at the next boot.',
        'Apply', { confirmClass: 'primary', fields: [
            { key: 'cpus', label: 'Cores', type: 'number', value: r.cpus || 4, min: 1, max: lim.cores },
            { key: 'ram', label: 'RAM in MB', type: 'number', value: r.ram || 4096, min: 512, max: lim.ram, step: 256 },
            { key: 'disk', label: 'Disk in GB', type: 'number', value: r.disk || 20, min: r.disk || 1, max: 2048 },
            { key: 'restart', label: running ? 'Restart the replica now to apply' : 'Start the replica afterwards', type: 'checkbox', value: running }
        ] })
    .then(function(f) {
        if (!f) return;
        var cpus = parseInt(f.cpus, 10), ram = parseInt(f.ram, 10), disk = parseInt(f.disk, 10);
        if (!(cpus >= 1) || !(ram >= 256) || !(disk >= 1)) return;
        if (r.disk && disk < r.disk) { showModal('Nested replica', 'The disk can only grow (it is ' + r.disk + ' GB now).', 'OK', { confirmClass: 'primary' }); return; }
        pendRep(vm.name, r.name, 'Resizing', 120000, function() {
            var x = findRep(vm.name, r.name);
            return !x || (x.cpus === cpus && x.ram === ram && x.disk === disk);
        });
        sendCmd('replicaResize', {vmIndex: idx, name: r.name, cpus: cpus, ram: ram, disk: disk, restart: !!f.restart});
    });
}

function makeCell(text, row, col, title) {
    var td = document.createElement('td');
    td.textContent = text;
    if (title) td.title = title;
    if (col === 4 || col === 5 || col === 6) td.className = 'num';
    /* GPU names from lspci run long: clip them so the actions stay in view */
    if (col === 7) { td.className = 'gpu-col'; if (!title) td.title = text; }

    /* an edited value the host has not echoed back yet */
    var vm = vms[row];
    var ep = vm && pendingFor('edit:' + vm.name);
    if (ep && ep.col === col) {
        var sp = document.createElement('span');
        sp.className = 'spinner';
        sp.title = 'Saving…';
        td.appendChild(sp);
    }

    /* Editable columns: 4=CPU, 5=RAM, 7=GPU, 8=Network */
    if (editModeRow === row && (col === 4 || col === 5 || col === 7 || col === 8)) {
        td.classList.add('editable');
        td.title = 'Click to edit';
        td.onclick = function(e) {
            e.stopPropagation();
            startInlineEdit(row, col, td);
        };
    }
    return td;
}

/* Which glyph an icon button shows: by the button's kind, or by the legacy
   emoji the caller passes for state variants (start / screen / check). */
var ICON_BY_CLASS = {
    start: 'i-play', 'connect-idd': 'i-monitor', vnc: 'i-nest', identity: 'i-id', add: 'i-plus',
    shutdown: 'i-power', stop: 'i-x', 'delete': 'i-trash', edit: 'i-pencil', restart: 'i-restart', dismiss: 'i-x'
};
var ICON_BY_GLYPH = {
    '▶️': 'i-play', '🖥️': 'i-screen', '✔️': 'i-check',
    '+': 'i-plus', 'desktop': 'i-monitor', '↻': 'i-restart', 'grid': 'i-grid', 'x': 'i-x', 'id': 'i-id'
};
function iconMarkup(cls, icon) {
    if (cls === 'ssh') return '<span class="mono">&gt;_</span>';
    if (icon === 'spinner') return '<span class="spinner"></span>';
    var id = ICON_BY_GLYPH[icon] || ICON_BY_CLASS[cls];
    return id ? '<svg class="ic" aria-hidden="true"><use href="#' + id + '"/></svg>' : icon;
}

/* ---- VM Selection ---- */

function selectVm(idx) {
    if (editingCell) commitInlineEdit();
    if (editModeRow >= 0 && editModeRow !== idx) editModeRow = -1;
    selectedVm = idx;
    renderVmTable();
    sendCmd('selectVm', { vmIndex: idx });
}

/* ---- Inline Editing ---- */

function toggleEditMode(row) {
    if (editingCell) commitInlineEdit();
    if (vms[row] && vms[row].running) return;
    editModeRow = (editModeRow === row) ? -1 : row;
    renderVmTable();
}

function startInlineEdit(row, col, td) {
    if (editingCell) commitInlineEdit();
    var vm = vms[row];
    if (!vm || vm.running) return;

    var oldValue;
    /* Lock cell width before swapping content to prevent column resize */
    var cellWidth = td.getBoundingClientRect().width;
    td.style.width = cellWidth + 'px';
    td.style.maxWidth = cellWidth + 'px';
    td.classList.add('editing');

    if (col === 7) {
        /* GPU combo */
        var sel = document.createElement('select');
        sel.innerHTML = '<option value="0">None</option><option value="1">Default GPU</option><option value="2">Try all</option>';
        sel.value = String(vm.gpuMode);
        sel.onclick = function(e) { e.stopPropagation(); };
        sel.onchange = function() { commitInlineEdit(); };
        sel.onblur = function() { setTimeout(commitInlineEdit, 100); };
        td.textContent = '';
        td.appendChild(sel);
        editingCell = { row: row, col: col, element: sel };
        sel.focus();
        setTimeout(function() { try { sel.showPicker(); } catch(e) {} }, 0);
    } else if (col === 8) {
        /* Network combo */
        var sel = document.createElement('select');
        sel.innerHTML = '<option value="0">None</option><option value="1">NAT</option><option value="2">External</option><option value="3">Internal</option>';
        sel.value = String(vm.networkMode);
        sel.onclick = function(e) { e.stopPropagation(); };
        sel.onchange = function() { commitInlineEdit(); };
        sel.onblur = function() { setTimeout(commitInlineEdit, 100); };
        td.textContent = '';
        td.appendChild(sel);
        editingCell = { row: row, col: col, element: sel };
        sel.focus();
        setTimeout(function() { try { sel.showPicker(); } catch(e) {} }, 0);
    } else {
        /* Text/number input */
        var inp = document.createElement('input');
        inp.type = 'number';
        inp.value = col === 4 ? String(vm.cpuCores) : String(vm.ramMb);
        inp.onkeydown = function(e) {
            if (e.key === 'Enter') commitInlineEdit();
            else if (e.key === 'Escape') cancelInlineEdit();
        };
        inp.onblur = function() { commitInlineEdit(); };
        td.textContent = '';
        td.appendChild(inp);
        inp.select();
        inp.focus();
        editingCell = { row: row, col: col, element: inp };
    }
}

function commitInlineEdit() {
    if (!editingCell) return;
    var el = editingCell.element;
    var row = editingCell.row;
    var col = editingCell.col;
    var value = el.value;
    editingCell = null;

    var field;
    if (col === 4) field = 'cpuCores';
    else if (col === 5) field = 'ramMb';
    else if (col === 7) field = 'gpuMode';
    else if (col === 8) field = 'networkMode';

    /* RAM must be 2 MB-aligned (HCS requirement): round an odd entry down by 1. */
    if (field === 'ramMb') {
        var mb = parseInt(value, 10);
        if (!isNaN(mb)) value = String(alignRamMb(mb));
    }

    if (field) {
        var vmName = vms[row] && vms[row].name, want = value;
        if (vmName) setPending('edit:' + vmName, { label: 'Saving', ttl: 8000, col: col, expect: function() {
            var v = findVm(vmName); return !v || String(v[field]) === String(want);
        } });
        sendCmd('editVm', { vmIndex: row, field: field, value: value });
        if (field === 'networkMode' && value === '2' && currentDefaultAdapter) {
            sendCmd('editVm', { vmIndex: row, field: 'netAdapter', value: currentDefaultAdapter });
        }
    }
}

function cancelInlineEdit() {
    editingCell = null;
    renderVmTable();
}

/* ---- Force Stop VM ---- */

function onStopVm(idx) {
    var vm = vms[idx];
    if (!vm) return;
    if (vm.isTemplate) {
        showModal(
            'Cancel Template Build',
            'Stopping a template build will delete the incomplete template "' + vm.name + '".\n\nAre you sure?',
            'Stop & Delete'
        ).then(function(confirmed) {
            if (confirmed) {
                pendVm(vm.name, 'Deleting', 120000, function() { return !findVm(vm.name); });
                sendCmd('stopVm', { vmIndex: idx });
                sendCmd('deleteVm', { vmIndex: idx });
            }
        });
    } else {
        if (localStorage.getItem('suppress_force_stop_warn') === '1') {
            pendVm(vm.name, 'Stopping', 60000, function() { var v = findVm(vm.name); return !v || !v.running; });
            sendCmd('stopVm', { vmIndex: idx });
        } else {
            showForceStopModal(idx);
        }
    }
}

function showForceStopModal(idx) {
    document.getElementById('modal-title').textContent = 'Force Stop';
    document.getElementById('modal-message').textContent =
        'Force Stop will immediately power-off "' + vms[idx].name + '" which may result in corruption of its data.';
    document.getElementById('modal-confirm-btn').textContent = 'Force Stop';

    var cb = document.getElementById('modal-dont-show');
    if (cb) { cb.checked = false; cb.parentElement.style.display = ''; }

    document.getElementById('modal-overlay').classList.add('active');
    pendingConfirm = { resolve: function(confirmed) {
        if (confirmed) {
            if (cb && cb.checked) localStorage.setItem('suppress_force_stop_warn', '1');
            var vn = vms[idx].name;
            pendVm(vn, 'Stopping', 60000, function() { var v = findVm(vn); return !v || !v.running; });
            sendCmd('stopVm', { vmIndex: idx });
        }
        if (cb) cb.parentElement.style.display = 'none';
    }};
}

/* ---- Delete VM ---- */

function onDeleteVm(idx) {
    var vm = vms[idx];
    if (!vm) return;
    showModal(
        'Confirm Delete',
        'Are you sure you want to delete VM "' + vm.name + '"?\n\nThis will permanently delete all disk data and snapshots.',
        'Delete'
    ).then(function(confirmed) {
        if (confirmed) {
            pendVm(vm.name, 'Deleting', 180000, function() { return !findVm(vm.name); });
            sendCmd('deleteVm', { vmIndex: idx });
        }
    });
}

/* ---- Snapshots ---- */

/* Parse select value string into {snapIndex, branchIndex} */
function parseSnapValue(val) {
    if (!val || val === 'current') return {snapIndex: -1, branchIndex: -1};
    if (val === 'base') return {snapIndex: -2, branchIndex: -1};
    if (val.substring(0, 5) === 'base-') return {snapIndex: -2, branchIndex: parseInt(val.substring(5))};
    var parts = val.split('-');
    if (parts.length === 1) return {snapIndex: parseInt(parts[0]), branchIndex: -1};
    return {snapIndex: parseInt(parts[0]), branchIndex: parseInt(parts[1])};
}

function makeSnapCell(vm, vmIdx) {
    var td = document.createElement('td');
    td.className = 'snap-cell';
    var snaps = vm.snapshots || [];
    var baseBranches = vm.baseBranches || [];
    var curSnap = vm.snapCurrent;       /* -2=base, -1=pre-snapshot, >=0=snapshot index */
    var curBranch = vm.snapCurrentBranch; /* branch index or -1 */
    var hasSn = vm.hasSnapshots;
    var sel = selectedSnap[vmIdx] || 'current';

    var snapPending = pendingFor('snap:' + vm.name);
    if (snapPending) {
        td.className = 'snap-cell busy';
        td.innerHTML = '<span class="hint">' + snapPending.label + '…</span><span class="spinner"></span>';
        return td;
    }
    /* what the tree looks like now: a snapshot action is done once it differs */
    var treeNow = JSON.stringify([vm.snapshots || [], vm.baseBranches || [], vm.hasSnapshots]);
    var snapPend = function(label) {
        setPending('snap:' + vm.name, { label: label, ttl: 120000, expect: function() {
            var v = findVm(vm.name);
            return !v || JSON.stringify([v.snapshots || [], v.baseBranches || [], v.hasSnapshots]) !== treeNow;
        } });
    };

    var snapWrap = document.createElement('span');
    snapWrap.className = 'snap-wrap';

    var select = document.createElement('select');
    select.className = 'snap-select';
    select.disabled = vm.running;

    function addOpt(value, text, selected) {
        var o = document.createElement('option');
        o.value = value;
        o.textContent = text;
        if (selected) o.selected = true;
        select.appendChild(o);
    }

    if (!hasSn) {
        addOpt('current', 'No snapshots', true);
    } else {
        /*  Tree with multiple branches per node:
         *    Current (base, branch 1)
         *    \u251C Base                       <- new branch
         *    \u2502 \u251C branch 1 (date)     <- resume
         *    \u2502 \u2514 branch 2 (date)     <- resume
         *    \u251C Snapshot A (date)           <- new branch
         *    \u2502 \u2514 branch 1 (date)     <- resume
         *    \u2514 Snapshot B (date)           <- new branch
         */

        /* "Current" — resume whatever is active */
        addOpt('current', 'Current', sel === 'current');

        /* Base + its branches */
        addOpt('base', '\u251C Base [create new child branch]', sel === 'base');
        baseBranches.forEach(function(br, b) {
            var brChar = (b === baseBranches.length - 1) ? '\u2514' : '\u251C';
            var label = '\u2502\u00A0\u00A0' + brChar + ' ' + (br.name || 'branch ' + (b + 1));
            if (br.date) label += ' (' + br.date + ')';
            if (br.sizeGb) label += ' [' + br.sizeGb + ' GB]';
            addOpt('base-' + b, label, sel === 'base-' + b);
        });

        /* Snapshots + their branches */
        snaps.forEach(function(snap, i) {
            var isLast = (i === snaps.length - 1);
            var treePfx = isLast ? '\u2514 ' : '\u251C ';
            var contPfx = isLast ? '\u00A0\u00A0\u00A0' : '\u2502\u00A0\u00A0';
            var branches = snap.branches || [];

            addOpt(String(i), treePfx + snap.name + ' (' + snap.date + ') [create new child branch]', sel === String(i));

            branches.forEach(function(br, b) {
                var brChar = (b === branches.length - 1) ? '\u2514' : '\u251C';
                var label = contPfx + brChar + ' ' + (br.name || 'branch ' + (b + 1));
                if (br.date) label += ' (' + br.date + ')';
                if (br.sizeGb) label += ' [' + br.sizeGb + ' GB]';
                addOpt(i + '-' + b, label, sel === i + '-' + b);
            });
        });
    }

    select.onchange = function(e) {
        e.stopPropagation();
        selectedSnap[vmIdx] = select.value;
        renderVmTable();
    };
    snapWrap.appendChild(select);

    /* Chain overlay — shows selected path when dropdown is closed */
    if (hasSn) {
        var p = parseSnapValue(sel);
        var chainText = '';
        if (sel === 'current') {
            /* Show the currently active chain */
            if (curSnap >= 0 && snaps[curSnap]) {
                chainText = 'base \u2192 ' + snaps[curSnap].name;
                if (curBranch >= 0 && snaps[curSnap].branches && snaps[curSnap].branches[curBranch])
                    chainText += ' \u2192 ' + (snaps[curSnap].branches[curBranch].name || 'branch ' + (curBranch + 1));
            } else if (curSnap === -2) {
                chainText = 'base';
                if (curBranch >= 0 && baseBranches[curBranch])
                    chainText += ' \u2192 ' + (baseBranches[curBranch].name || 'branch ' + (curBranch + 1));
            } else {
                chainText = 'base';
            }
        } else if (p.snapIndex === -2) {
            chainText = 'base';
            if (p.branchIndex >= 0 && baseBranches[p.branchIndex])
                chainText += ' \u2192 ' + (baseBranches[p.branchIndex].name || 'branch ' + (p.branchIndex + 1));
            else
                chainText += ' [create new child branch]';
        } else if (p.snapIndex >= 0 && snaps[p.snapIndex]) {
            chainText = 'base \u2192 ' + snaps[p.snapIndex].name;
            if (p.branchIndex >= 0 && snaps[p.snapIndex].branches && snaps[p.snapIndex].branches[p.branchIndex])
                chainText += ' \u2192 ' + (snaps[p.snapIndex].branches[p.branchIndex].name || 'branch ' + (p.branchIndex + 1));
            else
                chainText += ' [create new child branch]';
        }
        var overlay = document.createElement('span');
        overlay.className = 'snap-overlay';
        overlay.textContent = chainText;
        snapWrap.appendChild(overlay);
    }
    td.appendChild(snapWrap);

    /* Take snapshot button — only when stopped */
    var takeBtn = document.createElement('button');
    takeBtn.className = 'snap-btn';
    takeBtn.innerHTML = '<svg class="ic"><use href="#i-plus"/></svg>';
    takeBtn.title = 'Take snapshot';
    takeBtn.disabled = vm.running;
    takeBtn.onclick = function(e) {
        e.stopPropagation();
        var defaultName = 'Snapshot ' + (snaps.length + 1);
        showModal('New Snapshot', 'Create a new snapshot of the base disk. Snapshots are frozen points in time that you can create independent branches from.', 'Create', {
            confirmClass: 'primary',
            input: { label: 'Snapshot name:', value: defaultName }
        }).then(function(result) {
            if (result === false) return;
            snapPend('Taking the snapshot');
            sendCmd('snapTake', { vmIndex: vmIdx, name: result });
        });
    };
    td.appendChild(takeBtn);

    /* Delete button — context-sensitive */
    var parsed = parseSnapValue(sel);
    if (!vm.running && parsed.snapIndex >= 0) {
        var delBtn = document.createElement('button');
        delBtn.className = 'snap-btn danger';
        delBtn.innerHTML = '<svg class="ic"><use href="#i-x"/></svg>';

        if (parsed.branchIndex >= 0) {
            /* Delete a single branch */
            delBtn.title = 'Delete branch';
            delBtn.onclick = function(e) {
                e.stopPropagation();
                showModal('Delete Branch',
                    'Delete this branch? The snapshot will be kept.',
                    'Delete'
                ).then(function(confirmed) {
                    if (confirmed) {
                        snapPend('Deleting the branch');
                        sendCmd('snapDeleteBranch', { vmIndex: vmIdx, snapIndex: parsed.snapIndex, branchIndex: parsed.branchIndex });
                        selectedSnap[vmIdx] = 'current';
                    }
                });
            };
        } else {
            /* Delete entire snapshot + all branches */
            delBtn.title = 'Delete snapshot';
            delBtn.onclick = function(e) {
                e.stopPropagation();
                var snapName = snaps[parsed.snapIndex] ? snaps[parsed.snapIndex].name : '';
                showModal('Delete Snapshot',
                    'Delete snapshot "' + snapName + '" and all its branches?',
                    'Delete'
                ).then(function(confirmed) {
                    if (confirmed) {
                        snapPend('Deleting the snapshot');
                        sendCmd('snapDelete', { vmIndex: vmIdx, snapIndex: parsed.snapIndex });
                        selectedSnap[vmIdx] = 'current';
                    }
                });
            };
        }
        td.appendChild(delBtn);
    }

    /* Delete button for base branches */
    if (!vm.running && parsed.snapIndex === -2 && parsed.branchIndex >= 0) {
        var delBrBtn = document.createElement('button');
        delBrBtn.className = 'snap-btn danger';
        delBrBtn.innerHTML = '<svg class="ic"><use href="#i-x"/></svg>';
        delBrBtn.title = 'Delete base branch';
        delBrBtn.onclick = function(e) {
            e.stopPropagation();
            showModal('Delete Branch',
                'Delete this base branch?',
                'Delete'
            ).then(function(confirmed) {
                if (confirmed) {
                    snapPend('Deleting the branch');
                    sendCmd('snapDeleteBranch', { vmIndex: vmIdx, snapIndex: -2, branchIndex: parsed.branchIndex });
                    selectedSnap[vmIdx] = 'current';
                }
            });
        };
        td.appendChild(delBrBtn);
    }

    /* Rename button — when a snapshot or branch is selected */
    if (!vm.running && parsed.snapIndex !== -1) {
        var currentName = '';
        if (parsed.snapIndex === -2 && parsed.branchIndex >= 0 && baseBranches[parsed.branchIndex]) {
            currentName = baseBranches[parsed.branchIndex].name || '';
        } else if (parsed.snapIndex >= 0 && snaps[parsed.snapIndex]) {
            if (parsed.branchIndex >= 0) {
                var br = snaps[parsed.snapIndex].branches && snaps[parsed.snapIndex].branches[parsed.branchIndex];
                currentName = br ? br.name || '' : '';
            } else {
                currentName = snaps[parsed.snapIndex].name || '';
            }
        }
        if (currentName || parsed.snapIndex >= 0) {
            var renBtn = document.createElement('button');
            renBtn.className = 'snap-btn';
            renBtn.innerHTML = '<svg class="ic"><use href="#i-pencil"/></svg>';
            renBtn.title = 'Rename';
            renBtn.onclick = function(e) {
                e.stopPropagation();
                showModal('Rename', 'Enter a new name:', 'Rename', {
                    confirmClass: 'primary',
                    input: { label: 'Name:', value: currentName }
                }).then(function(result) {
                    if (result === false || result === currentName) return;
                    var cmd = { vmIndex: vmIdx, snapIndex: parsed.snapIndex, name: result };
                    if (parsed.branchIndex >= 0) cmd.branchIndex = parsed.branchIndex;
                    snapPend('Renaming');
                    sendCmd('snapRename', cmd);
                });
            };
            td.appendChild(renBtn);
        }
    }

    return td;
}

/* ---- Log ---- */

function appendLog(msg) {
    var panel = document.getElementById('log-panel');
    var div = document.createElement('div');
    div.className = 'log-line';
    div.textContent = msg;
    panel.appendChild(div);
    panel.scrollTop = panel.scrollHeight;
}

/* ---- Prerequisite check ---- */

function onPrereqRequired() {
    document.getElementById('prereq-message').innerHTML =
        'Nestbox requires the <strong>Virtual Machine Platform</strong> Windows feature to create and run VMs. This feature is not currently enabled.';
    document.getElementById('prereq-buttons').innerHTML =
        '<button onclick="document.getElementById(\'prereq-overlay\').classList.remove(\'active\')">Cancel</button>' +
        '<button class="primary" onclick="enableFeature()">Enable</button>';
    document.getElementById('prereq-buttons').style.display = '';
    document.getElementById('prereq-overlay').classList.add('active');
}

function onPrereqReboot() {
    document.getElementById('prereq-message').innerHTML =
        '<strong>Virtual Machine Platform</strong> has been enabled but a reboot is required before VMs can be created or started.';
    document.getElementById('prereq-buttons').innerHTML =
        '<button onclick="document.getElementById(\'prereq-overlay\').classList.remove(\'active\')">Later</button>' +
        '<button class="primary" onclick="sendCmd(\'enableFeatureReboot\')">Reboot Now</button>';
    document.getElementById('prereq-buttons').style.display = '';
    document.getElementById('prereq-overlay').classList.add('active');
}

function enableFeature() {
    document.getElementById('prereq-message').innerHTML =
        'Enabling <strong>Virtual Machine Platform</strong>. This may take a minute...' +
        '<div class="prereq-progress"><div class="prereq-progress-bar" id="prereq-bar"></div></div>' +
        '<div class="prereq-pct" id="prereq-pct">0%</div>';
    document.getElementById('prereq-buttons').style.display = 'none';
    sendCmd('enableFeature');
}

function onPrereqProgress(msg) {
    var bar = document.getElementById('prereq-bar');
    var pctEl = document.getElementById('prereq-pct');
    if (bar) bar.style.width = msg.pct + '%';
    if (pctEl) pctEl.textContent = msg.pct + '%';
}

function onPrereqResult(msg) {
    if (msg.ok && !msg.reboot) {
        document.getElementById('prereq-overlay').classList.remove('active');
    } else if (msg.ok && msg.reboot) {
        document.getElementById('prereq-message').innerHTML =
            '<strong>Virtual Machine Platform</strong> has been enabled. A reboot is required for the change to take effect.';
        document.getElementById('prereq-buttons').innerHTML =
            '<button onclick="document.getElementById(\'prereq-overlay\').classList.remove(\'active\')">Later</button>' +
            '<button class="primary" onclick="sendCmd(\'enableFeatureReboot\')">Reboot Now</button>';
        document.getElementById('prereq-buttons').style.display = '';
    } else {
        document.getElementById('prereq-message').innerHTML =
            'Failed to enable <strong>Virtual Machine Platform</strong>.<br><br>' +
            'Try enabling it manually:<br>' +
            'Settings &gt; System &gt; Optional Features &gt; More Windows Features &gt; Virtual Machine Platform';
        document.getElementById('prereq-buttons').innerHTML =
            '<button onclick="document.getElementById(\'prereq-overlay\').classList.remove(\'active\')">Close</button>';
        document.getElementById('prereq-buttons').style.display = '';
    }
}

/* ---- Modal ---- */

function showModal(title, message, confirmText, opts) {
    document.getElementById('modal-title').textContent = title;
    document.getElementById('modal-message').textContent = message;
    var confirmBtn = document.getElementById('modal-confirm-btn');
    confirmBtn.textContent = confirmText || 'Confirm';
    confirmBtn.className = (opts && opts.confirmClass) || 'danger';
    var cb = document.getElementById('modal-dont-show');
    if (cb) cb.parentElement.style.display = 'none';
    var inputRow = document.getElementById('modal-input-row');
    var inputEl = document.getElementById('modal-input');
    if (opts && opts.input) {
        inputRow.style.display = 'block';
        inputEl.value = opts.input.value || '';
        if (opts.input.label) document.getElementById('modal-input-label').textContent = opts.input.label;
        inputEl.onkeydown = function(e) { if (e.key === 'Enter') modalResolve(true); };
        inputEl.oninput = function() {
            /* Strip characters that would break the INI-style .dat file */
            var clean = inputEl.value.replace(/[\n\r\t\[\]\\]/g, '');
            if (clean !== inputEl.value) inputEl.value = clean;
        };
        inputEl.maxLength = 127;
        setTimeout(function() { inputEl.select(); inputEl.focus(); }, 50);
    } else {
        inputRow.style.display = 'none';
    }
    /* fields: [{key, label, type: text|number|checkbox|select, value, min, max,
       step, options: [{value, label}]}] - a small form; the promise then
       resolves to {key: value} (or false). */
    var fieldsEl = document.getElementById('modal-fields');
    fieldsEl.innerHTML = '';
    if (opts && opts.fields) {
        opts.fields.forEach(function(f) {
            var row = document.createElement('label');
            var input = document.createElement(f.type === 'select' ? 'select' : 'input');
            if (f.type !== 'select') input.type = f.type || 'text';
            input.dataset.key = f.key;
            if (f.type === 'checkbox') {
                row.className = 'check modal-field modal-field-check';
                input.checked = !!f.value;
                row.appendChild(input);
                row.appendChild(document.createTextNode(' ' + f.label));
            } else if (f.type === 'select') {
                row.className = 'modal-field modal-field-text';
                var slab = document.createElement('span');
                slab.className = 'field-label';
                slab.textContent = f.label;
                (f.options || []).forEach(function(o) {
                    var op = document.createElement('option');
                    op.value = o.value;
                    op.textContent = o.label;
                    if (o.value === f.value) op.selected = true;
                    input.appendChild(op);
                });
                row.appendChild(slab);
                row.appendChild(input);
            } else {
                row.className = 'modal-field' + (f.type === 'text' || !f.type ? ' modal-field-text' : '');
                var lab = document.createElement('span');
                lab.className = 'field-label';
                lab.textContent = f.label;
                input.value = f.value == null ? '' : f.value;
                if (f.min != null) input.min = f.min;
                if (f.max != null) input.max = f.max;
                if (f.step) input.step = f.step;
                input.spellcheck = false;
                input.onkeydown = function(e) { if (e.key === 'Enter') modalResolve(true); };
                row.appendChild(lab);
                row.appendChild(input);
            }
            fieldsEl.appendChild(row);
        });
        fieldsEl.style.display = '';
        setTimeout(function() {
            var first = fieldsEl.querySelector('input');
            if (first) { first.focus(); if (first.type === 'text') first.select(); }
        }, 50);
    } else {
        fieldsEl.style.display = 'none';
    }
    document.getElementById('modal-overlay').classList.add('active');

    return new Promise(function(resolve) {
        pendingConfirm = { resolve: resolve, hasInput: !!(opts && opts.input), hasFields: !!(opts && opts.fields) };
    });
}

function modalResolve(result) {
    document.getElementById('modal-overlay').classList.remove('active');
    if (pendingConfirm) {
        if (result && pendingConfirm.hasFields) {
            var values = {};
            document.querySelectorAll('#modal-fields input, #modal-fields select').forEach(function(el) {
                values[el.dataset.key] = el.type === 'checkbox' ? el.checked : el.value;
            });
            pendingConfirm.resolve(values);
        } else if (result && pendingConfirm.hasInput) {
            pendingConfirm.resolve(document.getElementById('modal-input').value);
        } else {
            pendingConfirm.resolve(result);
        }
        pendingConfirm = null;
    }
}

/* ---- Minimum size reporting ---- */

function reportMinSize() {
    var minW = 0;

    /* Measure <table> elements directly — they always report true natural width */
    var tables = document.querySelectorAll('table');
    tables.forEach(function(t) {
        if (t.scrollWidth > minW) minW = t.scrollWidth;
    });

    /* Add wrapper border (2px) + body padding (24px) */
    minW += 28;

    /* Height: sum of all sections at minimum height (log just needs ~100px) */
    var minH = 0;
    var sections = document.querySelectorAll('section');
    sections.forEach(function(s, i) {
        if (i < sections.length - 1) {
            minH += s.scrollHeight + 12;
        } else {
            minH += 100;
        }
    });
    minH += 16;

    sendCmd('setMinSize', { width: minW, height: minH });
}

/* ---- Init ---- */
/* Signal to C that the UI is ready */
sendCmd('uiReady');

/* Report min size once layout is complete (covers case with no VMs) */
setTimeout(function() {
    if (!minSizeReported) {
        minSizeReported = true;
        reportMinSize();
    }
}, 300);
