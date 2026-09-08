#pragma once
#include "ihs_timer.h"
#include <ihslib/client.h>

// Request/cancel must be called outside ihslib callbacks. Both drain earlier
// streaming callbacks before returning. No client/base lock may be held.
void LunarIHSStreamingCancel(IHS_Client *client);
void LunarIHSTimerLock(IHS_Timer *timer);
void LunarIHSTimerUnlock(IHS_Timer *timer);
IHS_TimerTask *LunarIHSTimerStartLocked(IHS_Timer *, IHS_TimerRunFunction *,
                                      IHS_TimerEndFunction *, uint64_t, void *);
void LunarIHSTimerStopLocked(IHS_TimerTask *task);
void LunarIHSTimerRemoveLocked(IHS_TimerTask *task);

// Direct discovery still requires a real host status response before pairing.
bool LunarIHSDiscoverAddress(IHS_Client *client, const char *address);
