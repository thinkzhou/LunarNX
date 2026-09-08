// Same production implementation; expose only the deadline for deterministic replay.
#include "../src/steamlink/ihs_authorization.c"
void ExpireSteamAuthorization(IHS_Client *client) {
    LunarIHSTimerLock(client->timers);
    IHS_TimerTask *task = client->taskHandles.authorization;
    if (task) {
        IHS_AuthorizationState *state = IHS_TimerTaskGetContext(task);
        state->deadline = 0;
        // Keep the real timer's normal execution and cleanup path.
    }
    LunarIHSTimerUnlock(client->timers);
}
