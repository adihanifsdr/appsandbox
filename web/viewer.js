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

function sendCmd(action, data) {
    var msg = Object.assign({ action: action }, data || {});
    if (window.chrome && window.chrome.webview) window.chrome.webview.postMessage(msg);
}
function setStatus(s) { document.getElementById('vnc-status').textContent = s; }

/* One RFB into `container` on bridge port `port`; `onStatus` gets the state
   text. Returns a promise for the RFB. */
function makeRfb(container, port, onStatus, lostText) {
    container.innerHTML = '';
    onStatus('connecting…');
    return window.NoVNC.then(function(RFB) {
        var r = new RFB(container, wsUrl(port), { wsProtocols: ['binary'] });
        r.scaleViewport = scaled;
        r.resizeSession = false;
        r.background = '#000';
        r.addEventListener('connect', function() { onStatus('connected'); });
        r.addEventListener('disconnect', function(e) {
            var clean = e && e.detail && e.detail.clean;
            onStatus(clean ? 'disconnected' : lostText, r);
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
    makeRfb(document.getElementById('vnc-screen'), wsPort, function(s, from) {
        if (from && rfb && from !== rfb) return;   /* superseded by a reconnect */
        setStatus(s);
    }, nested ? 'connection lost (is the replica running?)' : 'connection lost')
    .then(function(r) { rfb = r; r.focus(); })
    .catch(function(e) { setStatus('viewer failed to load: ' + e); });
}

/* ---- grid ---- */
function tileConnect(t) {
    if (t.rfb) { try { t.rfb.disconnect(); } catch (e) {} t.rfb = null; }
    makeRfb(t.el.querySelector('.tile-screen'), t.ws, function(s, from) {
        if (from && t.rfb && from !== t.rfb) return;
        t.el.querySelector('.tile-status').textContent = s;
    }, 'connection lost')
    .then(function(r) { t.rfb = r; })
    .catch(function(e) { t.el.querySelector('.tile-status').textContent = 'viewer failed to load: ' + e; });
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
        var t = { name: p[0] || 'replica', ws: p[1] || '', port: parseInt(p[2], 10) || 5900, rfb: null };
        var el = document.createElement('div');
        el.className = 'tile';
        el.innerHTML = '<div class="tile-bar"><svg class="ic"><use href="#i-nest"/></svg>' +
                       '<span class="tile-name"></span><span class="tile-status mono"></span><span class="vnc-spacer"></span>' +
                       '<button class="t-own" title="Open this replica in its own window">Own window</button>' +
                       '<button class="t-reconnect" title="Connect this tile again">Reconnect</button></div>' +
                       '<div class="tile-screen"></div>';
        el.querySelector('.tile-name').textContent = t.name;
        el.querySelector('.t-own').onclick = function(e) { e.stopPropagation(); sendCmd('vncOpen', { vmIndex: vmIndex, port: t.port, name: t.name }); };
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
function vncReplica(action) {
    sendCmd(action, { vmIndex: vmIndex, name: replicaName });
    setStatus(action === 'replicaStop' ? 'stopping the replica…'
                                       : 'restarting the replica… (Reconnect once it is back up)');
}
function vncClose() {
    if (rfb) { try { rfb.disconnect(); } catch (e) {} rfb = null; }
    tiles.forEach(function(t) { if (t.rfb) { try { t.rfb.disconnect(); } catch (e) {} t.rfb = null; } });
    sendCmd('viewerClose');
}

if (grid) {
    document.getElementById('vnc-icon').setAttribute('href', '#i-grid');
    document.getElementById('vnc-title').textContent = vmName + ' / replicas';
    document.title = vmName + ' - replicas - Nestbox';
    ['vnc-external-btn', 'vnc-restart-btn', 'vnc-stop-btn'].forEach(function(id) { document.getElementById(id).style.display = 'none'; });
    document.getElementById('vnc-screen').hidden = true;
    buildGrid();
} else {
    document.getElementById('vnc-title').textContent = nested
        ? vmName + ' / ' + replicaName
        : vmName + ' (guest VNC :' + guestPort + ')';
    document.getElementById('vnc-stop-btn').style.display = nested ? '' : 'none';
    document.getElementById('vnc-restart-btn').style.display = nested ? '' : 'none';
    document.title = (nested ? vmName + ' / ' + replicaName : vmName) + ' - Nestbox';
    connect();
}
