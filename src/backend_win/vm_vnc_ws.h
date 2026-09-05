#ifndef VM_VNC_WS_H
#define VM_VNC_WS_H
/*
 * vm_vnc_ws.h - WebSocket bridge for the in-app VNC viewer.
 *
 * The GUI embeds noVNC (web/novnc.js), which speaks RFB over a WebSocket.
 * vm_vnc_ws_port() binds 127.0.0.1:<ephemeral>, answers the WebSocket
 * handshake (binary subprotocol) and relays each connection to
 * 127.0.0.1:<target_port> - the VNC tunnel vm_vnc_proxy_start() already
 * opened to the guest (the replica's console, or any VNC server on the
 * guest's port 5900). One bridge per target port, reused on later calls.
 * Returns the bound port, 0 on failure.
 */
#include <windows.h>
#include "asb_core.h"

ASB_API DWORD vm_vnc_ws_port(DWORD target_port);

#endif
