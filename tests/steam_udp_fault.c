// Test-only receive failure injection around the same POSIX transport used by
// the production desktop replay. Switch production still uses its normal source.
#include <stdatomic.h>
#define IHS_UDPSocketReceive IHS_OriginalUDPSocketReceive
#include "../vendor/ihslib/src/platforms/ihs_udp_posix.c"
#undef IHS_UDPSocketReceive
static atomic_bool fail_next_receive;
void FailNextSteamReceive(void) { atomic_store(&fail_next_receive, true); }
int IHS_UDPSocketReceive(IHS_UDPSocket *socket, IHS_UDPPacket *packet) {
    if (atomic_exchange(&fail_next_receive, false)) return -1;
    return IHS_OriginalUDPSocketReceive(socket, packet);
}
