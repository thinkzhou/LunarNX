// Stop HID callbacks before upstream destroys the channels they may send on.
// Caller must disconnect/join the session worker before destruction.
#define IHS_SessionDestroy IHS_OriginalSessionDestroy
#define IHS_SessionConnect IHS_OriginalSessionConnect
#include "../../vendor/ihslib/src/session/session.c"
#undef IHS_SessionDestroy
#undef IHS_SessionConnect

static void LunarSessionInitialized(IHS_Base *base, void *context) {
    // The POSIX socket defaults to a blocking receive. Unlike the discovery
    // client, upstream sessions never set a timeout, so interruption cannot
    // wake the receive worker if the host has stopped sending packets.
    IHS_UDPSocketSetBlocking(base->socket, true);
    IHS_UDPSocketSetRecvTimeout(base->socket, 10000 /* microseconds: 10ms */);
    SessionInitialized(base, context);
}

bool IHS_SessionConnect(IHS_Session *session) {
    static const IHS_BaseRunCallbacks callbacks = {
        .initialized = LunarSessionInitialized,
        .finalized = SessionFinalized,
    };
    IHS_BaseSetRunCallbacks(&session->base, &callbacks, NULL);
    return IHS_OriginalSessionConnect(session);
}

void IHS_SessionDestroy(IHS_Session *session) {
    if (session->hidManager->pollTimer) {
        // StopImmediate waits for any executing timer callback via timer->mutex.
        IHS_TimerTaskStopImmediate(session->hidManager->pollTimer);
        session->hidManager->pollTimer = NULL;
    }
    IHS_HIDManagerCloseAll(session->hidManager);
    IHS_OriginalSessionDestroy(session);
}
