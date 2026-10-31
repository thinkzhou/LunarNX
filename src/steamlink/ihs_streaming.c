#include "client/client_pri.h"
#include "ihs_streaming_support.h"

// The pinned implementation protects handle writes with base->lock but reads
// and frees the context concurrently. Serialize ALL streaming access on the
// timer lock instead. Only these direct calls are replaced, not BaseSend's
// socket lifetime lock. Locked operations also work with nonrecursive mutexes.
#define IHS_ClientStreamingRequest OriginalStreamingRequest
#define IHS_ClientStreamingCallback OriginalStreamingCallback
#define IHS_BaseLock(base) ((void)(base))
#define IHS_BaseUnlock(base) ((void)(base))
#define IHS_TimerTaskStart LunarIHSTimerStartLocked
#define IHS_TimerTaskStop LunarIHSTimerStopLocked
#include "../../vendor/ihslib/src/client/streaming.c"
#undef IHS_ClientStreamingRequest
#undef IHS_ClientStreamingCallback
#undef IHS_BaseLock
#undef IHS_BaseUnlock
#undef IHS_TimerTaskStart
#undef IHS_TimerTaskStop

bool IHS_ClientStreamingRequest(IHS_Client *client, const IHS_HostInfo *host,
                                const IHS_StreamingRequest *request) {
    LunarIHSTimerLock(client->timers);
    bool result = OriginalStreamingRequest(client, host, request);
    LunarIHSTimerUnlock(client->timers);
    return result;
}
void IHS_ClientStreamingCallback(IHS_Client *client, const IHS_SocketAddress *address,
                                CMsgRemoteClientBroadcastHeader *header, ProtobufCMessage *message) {
    LunarIHSTimerLock(client->timers);
    IHS_TimerTask *task = client->taskHandles.streaming;
    if (task) {
        IHS_StreamingState *request = IHS_TimerTaskGetContext(task);
        // Reject stale responses before they can refresh the request timeout.
        bool matches = header->has_client_id && header->client_id == request->host.clientId;
        if (header->msg_type == k_ERemoteDeviceStreamingResponse)
            matches = matches && ((CMsgRemoteDeviceStreamingResponse *) message)->request_id == request->requestId;
        else if (header->msg_type == k_ERemoteDeviceProofRequest)
            matches = matches && ((CMsgRemoteDeviceProofRequest *) message)->request_id == request->requestId;
        else matches = false;
        if (matches) OriginalStreamingCallback(client, address, header, message);
    }
    LunarIHSTimerUnlock(client->timers);
}
void LunarIHSStreamingCancel(IHS_Client *client) {
    LunarIHSTimerLock(client->timers);
    if (client->taskHandles.streaming)
        LunarIHSTimerRemoveLocked(client->taskHandles.streaming);
    LunarIHSTimerUnlock(client->timers);
}


bool LunarIHSDiscoverAddress(IHS_Client *client, const char *text) {
    IHS_SocketAddress address = {0};
    address.port = 27036;
    if (!IHS_IPAddressFromString(&address.ip, text) || address.ip.family != IHS_IPAddressFamilyIPv4)
        return false;
    CMsgRemoteClientBroadcastDiscovery message = CMSG_REMOTE_CLIENT_BROADCAST_DISCOVERY__INIT;
    message.has_seq_num = true;
    message.seq_num = 0;
    return IHS_ClientSend(client, address, k_ERemoteClientBroadcastMsgDiscovery, &message.base);
}
