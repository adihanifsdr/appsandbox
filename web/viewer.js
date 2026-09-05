/* Nestbox - screen viewer window (noVNC over the loopback WebSocket bridge).
   The host opens one window per screen and hands the parameters over in the
   query string (see viewer.html). Buttons talk to the host with the same
   actions the main page uses; "viewerClose" closes this window. */
'use strict';

var q = {};
location.search.replace(/^\?/, '').split('&').forEach(function(kv) {
    if (!kv) return;
    var p = kv.split('=');
    q[decodeURIComponent(p[0])] = decodeURIComponent((p[1] || '').replace(/\+/g, ' '));
});
var wsPort = parseInt(q.ws, 10) || 0;
var vmIndex = parseInt(q.vm, 10);
var guestPort = parseInt(q.port, 10) || 5900;
var replicaName = q.name || 'replica';
var vmName = q.vmName || '';
var nested = q.nested === '1';
var rfb = null, scaled = true;

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

function connect() {
    var screen = document.getElementById('vnc-screen');
    if (rfb) { try { rfb.disconnect(); } catch (e) {} rfb = null; }
    screen.innerHTML = '';
    setStatus('connecting…');
    window.NoVNC.then(function(RFB) {
        var r = new RFB(screen, 'ws://127.0.0.1:' + wsPort, { wsProtocols: ['binary'] });
        r.scaleViewport = scaled;
        r.resizeSession = false;
        r.background = '#000';
        r.addEventListener('connect', function() { setStatus('connected'); r.focus(); });
        r.addEventListener('disconnect', function(e) {
            var clean = e && e.detail && e.detail.clean;
            if (rfb !== r) return;   /* superseded by a reconnect */
            setStatus(clean ? 'disconnected' : (nested ? 'connection lost (is the replica running?)' : 'connection lost'));
        });
        r.addEventListener('credentialsrequired', function() {
            var pw = prompt('VNC password');
            r.sendCredentials({ password: pw || '' });
        });
        rfb = r;
    }).catch(function(e) {
        setStatus('viewer failed to load: ' + e);
    });
}

function vncSendCad() { if (rfb) rfb.sendCtrlAltDel(); }
function vncToggleScale() {
    scaled = !scaled;
    if (rfb) rfb.scaleViewport = scaled;
    document.getElementById('vnc-scale-btn').textContent = scaled ? 'Fit' : '1:1';
}
function vncReconnect() { connect(); }
function vncExternal() { sendCmd('vncConnect', { vmIndex: vmIndex, port: guestPort }); }
function vncReplica(action) {
    sendCmd(action, { vmIndex: vmIndex, name: replicaName });
    setStatus(action === 'replicaStop' ? 'stopping the replica…'
                                       : 'restarting the replica… (Reconnect once it is back up)');
}
function vncClose() {
    if (rfb) { try { rfb.disconnect(); } catch (e) {} rfb = null; }
    sendCmd('viewerClose');
}

document.getElementById('vnc-title').textContent = nested
    ? vmName + ' / ' + replicaName
    : vmName + ' (guest VNC :' + guestPort + ')';
document.getElementById('vnc-stop-btn').style.display = nested ? '' : 'none';
document.getElementById('vnc-restart-btn').style.display = nested ? '' : 'none';
document.title = (nested ? vmName + ' / ' + replicaName : vmName) + ' - Nestbox';
connect();
