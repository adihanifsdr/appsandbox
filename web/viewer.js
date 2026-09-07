/* Nestbox - screen viewer window (noVNC over the loopback WebSocket bridge).
   The host opens one window per screen - or one window for every running
   replica of a sandbox (grid mode) - and hands the parameters over in the
   query string (see viewer.html). Buttons talk to the host with the same
   actions the main page uses; "viewerClose" closes this window. */
'use strict';

var q = {};
location.search.replace(/^\?/, '').split('&').forEach(function(kv) {
    if (!kv) return;
    var p = kv.split('=');
    q[decodeURIComponent(p[0])] = decodeURIComponent((p[1] || '').replace(/\+/g, ' '));
});
var vmIndex = parseInt(q.vm, 10);
var vmName = q.vmName || '';
var grid = q.grid === '1';
var scaled = true;

/* Where a console's WebSocket is: a port (the Windows app's loopback bridge)
   or a path on the page's own server (the Linux host: /vnc/<port>, so one
   SSH tunnel carries the UI and every console). */
function wsUrl(ws) {
    ws = String(ws || '');
    if (/^\d+$/.test(ws)) return 'ws://127.0.0.1:' + ws;
    return (location.protocol === 'https:' ? 'wss://' : 'ws://') + location.host + ws;
}

/* single-console mode */
var wsPort = q.ws || '';
var guestPort = parseInt(q.port, 10) || 5900;
var replicaName = q.name || 'replica';
var nested = q.nested === '1';
/* what the screen belongs to: a nested replica (KVM guest) or a Steam seat
   (a user's Xvnc desktop on the sandbox itself); picks the Stop / Restart
   actions (replicaStop / seatStop ...) and the wording */
var kind = q.kind === 'seat' ? 'seat' : 'replica';
var rfb = null;

/* grid mode: [{name, ws, port, rfb, el, status}] */
var tiles = [];
var focusedTile = null;

/* same theme as the main window (remembered per machine) */
(function() {
    var t = null;
    try { t = localStorage.getItem('asb-theme'); } catch (e) {}
    document.documentElement.setAttribute('data-theme', t === 'light' ? 'light' : 'dark');
})();

/* Linux host: the page comes from tools/linux/host/nestbox over HTTP and the
   bar's actions (seatStop, vncConnect, ...) go to it on the same WebSocket
   the panel uses, /ws. The Windows app gets them through WebView2. */
var isWS = !(window.chrome && window.chrome.webview) && /^https?:/.test(location.protocol);
var hostWs = null, hostQueue = [];
function hostConnect() {
    hostWs = new WebSocket((location.protocol === 'https:' ? 'wss://' : 'ws://') + location.host + '/ws');
    hostWs.onopen = function() { var q = hostQueue; hostQueue = []; q.forEach(function(s) { hostWs.send(s); }); };
    hostWs.onclose = function() { hostWs = null; };
}
function sendCmd(action, data) {
    var msg = Object.assign({ action: action }, data || {});
    if (window.chrome && window.chrome.webview) window.chrome.webview.postMessage(msg);
    else if (isWS) {
        var s = JSON.stringify(msg);
        if (hostWs && hostWs.readyState === 1) hostWs.send(s);
        else { hostQueue.push(s); if (!hostWs) hostConnect(); }
    }
}
function setStatus(s) { document.getElementById('vnc-status').textContent = s; }

/* The overlay over a console: a spinner while connecting, the reason and a
   Reconnect button when the link is gone, a note while the host restarts
   the replica or seat. `null` hides it. */
function showState(el, state) {
    if (!el) return;
    if (!state) { el.hidden = true; el.innerHTML = ''; return; }
    el.innerHTML = '';
    if (state.spin) { var sp = document.createElement('span'); sp.className = 'spinner'; el.appendChild(sp); }
    if (state.error) { var lamp = document.createElement('span'); lamp.className = 'lamp-err'; el.appendChild(lamp); }
    var t = document.createElement('div'); t.className = 'state-title'; t.textContent = state.title; el.appendChild(t);
    if (state.text) { var x = document.createElement('div'); x.className = 'state-text'; x.textContent = state.text; el.appendChild(x); }
    if (state.action) {
        var b = document.createElement('button');
        b.textContent = state.action;
        b.onclick = function(e) { e.stopPropagation(); state.onAction(); };
        el.appendChild(b);
    }
    el.hidden = false;
}

/* One RFB into `container` on bridge port `port`; `onStatus` gets the state
   text, `stateEl` the overlay. Returns a promise for the RFB. */
function makeRfb(container, port, onStatus, lostText, stateEl, retry, what) {
    container.innerHTML = '';
    onStatus('connecting…');
    showState(stateEl, { spin: true, title: 'Connecting to ' + what + '…' });
    return window.NoVNC.then(function(RFB) {
        var r = new RFB(container, wsUrl(port), { wsProtocols: ['binary'] });
        r.scaleViewport = scaled;
        r.resizeSession = false;
        r.background = '#000';
        r.addEventListener('connect', function() { onStatus('connected', r); showState(stateEl, null); });
        r.addEventListener('disconnect', function(e) {
            var clean = e && e.detail && e.detail.clean;
            onStatus(clean ? 'disconnected' : lostText, r);
            showState(stateEl, { error: true, title: clean ? 'Disconnected' : 'Connection lost',
                                 text: clean ? 'The console closed the connection.' : lostText,
                                 action: 'Reconnect', onAction: retry });
        });
        r.addEventListener('credentialsrequired', function() {
            var pw = prompt('VNC password');
            r.sendCredentials({ password: pw || '' });
        });
        return r;
    });
}

/* ---- single console ---- */
function connect() {
    if (rfb) { try { rfb.disconnect(); } catch (e) {} rfb = null; }
    var stateEl = document.getElementById('vnc-state');
    makeRfb(document.getElementById('vnc-target'), wsPort, function(s, from) {
        if (from && rfb && from !== rfb) return;   /* superseded by a reconnect */
        setStatus(s);
    }, nested ? 'Is the ' + kind + ' running? Start it from the Nestbox window, then reconnect.' : 'The guest\'s VNC server went away.',
    stateEl, connect, nested ? replicaName : vmName)
    .then(function(r) { rfb = r; r.focus(); })
    .catch(function(e) {
        setStatus('viewer failed to load: ' + e);
        showState(stateEl, { error: true, title: 'The viewer failed to load', text: String(e), action: 'Try again', onAction: connect });
    });
}

/* ---- grid ---- */
function tileConnect(t) {
    if (t.rfb) { try { t.rfb.disconnect(); } catch (e) {} t.rfb = null; }
    var stateEl = t.el.querySelector('.screen-state');
    makeRfb(t.el.querySelector('.screen-target'), t.ws, function(s, from) {
        if (from && t.rfb && from !== t.rfb) return;
        t.el.querySelector('.tile-status').textContent = s;
    }, 'Is the ' + t.kind + ' still running?', stateEl, function() { tileConnect(t); }, t.name)
    .then(function(r) { t.rfb = r; })
    .catch(function(e) {
        t.el.querySelector('.tile-status').textContent = 'viewer failed to load: ' + e;
        showState(stateEl, { error: true, title: 'The viewer failed to load', text: String(e), action: 'Try again', onAction: function() { tileConnect(t); } });
    });
}
function focusTile(t) {
    focusedTile = t;
    tiles.forEach(function(x) { x.el.classList.toggle('focused', x === t); });
    if (t.rfb) t.rfb.focus();
}
function buildGrid() {
    var gridEl = document.getElementById('vnc-grid');
    var spec = (q.tiles || '').split(',').filter(Boolean);
    spec.forEach(function(s) {
        var p = s.split(':');
        /* name:ws path:port:kind[:replica it lives in] */
        var t = { name: p[0] || 'replica', ws: p[1] || '', port: parseInt(p[2], 10) || 5900, kind: p[3] === 'seat' ? 'seat' : 'replica',
                  inRep: p[4] ? decodeURIComponent(p[4]) : '', rfb: null };
        var el = document.createElement('div');
        el.className = 'tile';
        el.innerHTML = '<div class="tile-bar"><svg class="ic"><use href="#' + (t.kind === 'seat' ? 'i-screen' : 'i-nest') + '"/></svg>' +
                       '<span class="tile-name"></span><span class="tile-status mono"></span><span class="vnc-spacer"></span>' +
                       '<button class="t-own" title="Open this ' + t.kind + ' in its own window">Own window</button>' +
                       '<button class="t-reconnect" title="Connect this tile again">Reconnect</button></div>' +
                       '<div class="tile-screen"><div class="screen-target"></div><div class="screen-state" hidden></div></div>';
        el.querySelector('.tile-name').textContent = t.name;
        el.querySelector('.t-own').onclick = function(e) { e.stopPropagation(); sendCmd('vncOpen', { vmIndex: vmIndex, port: t.port, name: t.inRep ? t.name.slice(t.inRep.length + 1) : t.name, kind: t.kind, 'in': t.inRep }); };
        el.querySelector('.t-reconnect').onclick = function(e) { e.stopPropagation(); tileConnect(t); };
        el.addEventListener('mousedown', function() { focusTile(t); });
        gridEl.appendChild(el);
        t.el = el;
        tiles.push(t);
    });
    var cols = Math.max(1, Math.ceil(Math.sqrt(tiles.length)));
    if (tiles.length === 2) cols = 2;
    gridEl.style.setProperty('--cols', cols);
    gridEl.hidden = false;
    tiles.forEach(tileConnect);
    if (tiles.length) focusTile(tiles[0]);
    setStatus(tiles.length + (tiles.length === 1 ? ' console' : ' consoles'));
}

/* ---- bar ---- */
function vncSendCad() {
    var r = grid ? (focusedTile && focusedTile.rfb) : rfb;
    if (r) r.sendCtrlAltDel();
}
function vncSendAltTab() {
    var r = grid ? (focusedTile && focusedTile.rfb) : rfb;
    if (!r) return;
    /* Alt down, Tab down and up, Alt up: the guest's WM switches to its next window */
    r.sendKey(0xFFE9, 'AltLeft', true);
    r.sendKey(0xFF09, 'Tab', true);
    r.sendKey(0xFF09, 'Tab', false);
    r.sendKey(0xFFE9, 'AltLeft', false);
}
function vncToggleScale() {
    scaled = !scaled;
    if (rfb) rfb.scaleViewport = scaled;
    tiles.forEach(function(t) { if (t.rfb) t.rfb.scaleViewport = scaled; });
    document.getElementById('vnc-scale-btn').textContent = scaled ? 'Fit' : '1:1';
}
function vncReconnect() {
    if (grid) tiles.forEach(tileConnect);
    else connect();
}
function vncExternal() { sendCmd('vncConnect', { vmIndex: vmIndex, port: guestPort }); }
function vncReplica(what) {   /* 'Stop' | 'Restart' -> replicaStop / seatRestart ... */
    sendCmd(kind + what, { vmIndex: vmIndex, name: replicaName });
    setStatus(what === 'Stop' ? 'stopping the ' + kind + '…'
                              : 'restarting the ' + kind + '… (Reconnect once it is back up)');
    showState(document.getElementById('vnc-state'), what === 'Stop'
        ? { spin: true, title: 'Stopping ' + replicaName + '…', text: 'The ' + kind + ' is shutting down; this window can be closed.', action: 'Close', onAction: vncClose }
        : { spin: true, title: 'Restarting ' + replicaName + '…', text: 'Reconnect once the ' + kind + ' is back up (a seat in seconds, a replica in about a minute).', action: 'Reconnect', onAction: connect });
}
function vncClose() {
    if (rfb) { try { rfb.disconnect(); } catch (e) {} rfb = null; }
    tiles.forEach(function(t) { if (t.rfb) { try { t.rfb.disconnect(); } catch (e) {} t.rfb = null; } });
    sendCmd('viewerClose');
}

if (grid) {
    document.getElementById('vnc-icon').setAttribute('href', '#i-grid');
    document.getElementById('vnc-title').textContent = vmName + ' / screens';
    document.title = vmName + ' - screens - Nestbox';
    ['vnc-external-btn', 'vnc-restart-btn', 'vnc-stop-btn'].forEach(function(id) { document.getElementById(id).style.display = 'none'; });
    document.getElementById('vnc-screen').hidden = true;
    buildGrid();
} else {
    document.getElementById('vnc-title').textContent = nested
        ? vmName + ' / ' + replicaName
        : vmName + ' (guest VNC :' + guestPort + ')';
    var stopBtn = document.getElementById('vnc-stop-btn'), restartBtn = document.getElementById('vnc-restart-btn');
    stopBtn.style.display = nested ? '' : 'none';
    restartBtn.style.display = nested ? '' : 'none';
    if (kind === 'seat') {
        document.getElementById('vnc-icon').setAttribute('href', '#i-screen');
        stopBtn.textContent = 'Stop seat';
        stopBtn.title = 'Stop this seat: its XFCE session and Steam end (the user account stays)';
        restartBtn.textContent = 'Restart seat';
        restartBtn.title = 'Restart this seat (a fresh XFCE session)';
    }
    document.title = (nested ? vmName + ' / ' + replicaName : vmName) + ' - Nestbox';
    connect();
}
