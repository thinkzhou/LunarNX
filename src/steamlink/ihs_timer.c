// Pinned ihslib timer implementation plus narrowly scoped locked operations.
// Streaming state must share this lock with task execution/cleanup; a separate
// mutex around the callbacks would invert timer -> state versus state -> timer.
#include "../../vendor/ihslib/src/ihs_timer.c"
#include "ihs_streaming_support.h"

void LunarIHSTimerLock(IHS_Timer *timer) { IHS_MutexLock(timer->mutex); }
void LunarIHSTimerUnlock(IHS_Timer *timer) { IHS_MutexUnlock(timer->mutex); }

IHS_TimerTask *LunarIHSTimerStartLocked(IHS_Timer *timer, IHS_TimerRunFunction *run,
                                      IHS_TimerEndFunction *end, uint64_t timeout, void *context) {
    if (!timer->tasks) return NULL;
    IHS_TimerTask *task = (IHS_TimerTask *) IHS_QueueItemObtain(timer->tasks);
    if (!task) return NULL;
    task->timer = timer;
    task->run = run;
    task->end = end;
    task->context = context;
    task->nextExecution = IHS_TimerNow() + timeout;
    IHS_QueueAppend(timer->tasks, (IHS_QueueItem *) task);
    return task;
}
void LunarIHSTimerStopLocked(IHS_TimerTask *task) { task->nextExecution = 0; }
void LunarIHSTimerRemoveLocked(IHS_TimerTask *task) {
    IHS_Timer *timer = task->timer;
    IHS_TimerTask *removed = (IHS_TimerTask *) IHS_QueuePollBy(timer->tasks, ItemIdentical, task);
    if (!removed) return;
    TaskDestroy(removed, timer);
    IHS_QueueItemFree((IHS_QueueItem *) removed);
}
