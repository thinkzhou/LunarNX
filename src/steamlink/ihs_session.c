// Stop HID callbacks before upstream destroys the channels they may send on.
// Caller must disconnect/join the session worker before destruction.
#define IHS_SessionDestroy IHS_OriginalSessionDestroy
#include "../../vendor/ihslib/src/session/session.c"
#undef IHS_SessionDestroy

void IHS_SessionDestroy(IHS_Session *session) {
    if (session->hidManager->pollTimer) {
        // StopImmediate waits for any executing timer callback via timer->mutex.
        IHS_TimerTaskStopImmediate(session->hidManager->pollTimer);
        session->hidManager->pollTimer = NULL;
    }
    IHS_HIDManagerCloseAll(session->hidManager);
    IHS_OriginalSessionDestroy(session);
}
