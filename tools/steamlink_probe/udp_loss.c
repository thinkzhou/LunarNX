// Desktop-only fault injection. Never linked into the Switch build.
#define IHS_UDPSocketReceive IHS_OriginalUDPSocketReceive
#include "../../vendor/ihslib/src/platforms/ihs_udp_posix.c"
#undef IHS_UDPSocketReceive
#include "session/packet.h"
#include <stdio.h>
#include <time.h>

int IHS_UDPSocketReceive(IHS_UDPSocket *socket, IHS_UDPPacket *packet) {
    int result = IHS_OriginalUDPSocketReceive(socket, packet);
    if (result != 1 || !getenv("LUNAR_DROP_MEDIA_BURST") || packet->buffer.size < IHS_PACKET_HEADER_SIZE)
        return result;
    IHS_SessionPacketHeader header;
    if (!IHS_SessionPacketHeaderParse(&header, IHS_BufferPointer(&packet->buffer)) ||
        header.channelId < IHS_SessionChannelIdDataStart ||
        (header.type != IHS_SessionPacketTypeUnreliable && header.type != IHS_SessionPacketTypeUnreliableFrag))
        return result;
    static unsigned received;
    static uint64_t started_ms;
    static uint64_t last_burst;
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    uint64_t now_ms = (uint64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
    if (!started_ms) started_ms = now_ms;
    uint64_t elapsed = now_ms - started_ms;
    if (getenv("LUNAR_REPEAT_MEDIA_LOSS") && elapsed >= 30000 && elapsed % 30000 < 500) {
        uint64_t burst = elapsed / 30000;
        if (burst != last_burst) {
            last_burst = burst;
            fprintf(stderr, "LOSS TEST: repeated 500ms media outage burst=%llu\n", (unsigned long long)burst);
        }
        return 0;
    }
    ++received;
    if (received >= 600 && received < 3600) {
        if (received == 600) fprintf(stderr, "LOSS TEST: dropping 3000 media datagrams\n");
        return 0;
    }
    if (received == 3600) fprintf(stderr, "LOSS TEST: media delivery resumed\n");
    return result;
}
