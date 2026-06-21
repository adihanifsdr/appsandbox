/* asb_t_guest.c — guest side of the asb_transport test. Uses asb_transport (ivshmem backend):
 * listen ch1 -> accept -> send "hello" -> recv a line -> reply "<seq>:pong". Mirrors the agent's
 * connect/hello handshake. Build: cl /O2 asb_t_guest.c asb_transport.c /link ws2_32.lib setupapi.lib */
#include "../asb_transport.h"
#include <stdio.h>
#include <string.h>

int main(void) {
    AsbListener *l; AsbConn *c; char buf[256], reply[300], *colon; int n;

    if (asb_transport_init() != 0) { printf("init failed\n"); return 1; }
    printf("backend=%s\n", asb_transport_is_ivshmem() ? "ivshmem" : "hyperv"); fflush(stdout);

    l = asb_listen(ASB_CH_AGENT);
    if (!l) { printf("asb_listen(ch1) failed (directory/region present?)\n"); return 1; }
    printf("listening ch1; waiting for host connect (30s)...\n"); fflush(stdout);

    c = asb_accept(l, 30000);
    if (!c) { printf("accept timeout\n"); return 1; }
    printf("accepted; sending hello\n"); fflush(stdout);
    asb_send_line(c, "hello");

    n = asb_recv_line(c, buf, sizeof(buf));
    if (n <= 0) { printf("recv failed/closed (%d)\n", n); return 1; }
    printf("recv: '%s'  (expect '1:ping')\n", buf); fflush(stdout);

    colon = strchr(buf, ':');
    if (colon) { *colon = 0; snprintf(reply, sizeof(reply), "%s:pong", buf); }
    else strcpy(reply, "ack:pong");
    asb_send_line(c, reply);
    printf("sent: '%s'\n", reply); fflush(stdout);

    asb_close(c); asb_close_listener(l);
    printf("guest done\n");
    return 0;
}
