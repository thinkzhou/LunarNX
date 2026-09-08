// Narrow override of the pinned ihslib poll task: lock the stable managed slot
// before accessing its device, which a concurrent host close can destroy.
#define IHS_HIDManagerAddProvider IHS_OriginalHIDManagerAddProvider
#include "../../vendor/ihslib/src/hid/manager.c"
#undef IHS_HIDManagerAddProvider

static uint64_t LunarHIDPoll(int runCount, void *context) {
    (void)runCount;
    IHS_HIDManager *manager = context;
    if (!IHS_SessionInputEnabled(manager->session)) return HID_POLL_INTERVAL_MS;
    size_t count;
    IHS_HIDManagedDevice **devices = IHS_HIDManagerSnapshotOpenDevices(manager, &count);
    bool changed = false;
    for (size_t i = 0; i < count; ++i) {
        IHS_HIDManagedDevice *slot = devices[i];
        IHS_MutexLock(slot->lock);
        int result = 0;
        if (!slot->closed && slot->device && slot->device->cls->poll)
            result = slot->device->cls->poll(slot->device);
        IHS_MutexUnlock(slot->lock);
        if (result > 0) changed = true;
        if (result < 0) IHS_HIDManagedDeviceClose(slot);
    }
    free(devices);
    if (changed) IHS_SessionHIDSendReport(manager->session);
    return HID_POLL_INTERVAL_MS;
}

void IHS_HIDManagerAddProvider(IHS_HIDManager *manager, IHS_HIDProvider *provider) {
    provider->manager = manager;
    IHS_ArrayListAppend(&manager->providers, &provider);
    if (!manager->pollTimer && manager->session)
        manager->pollTimer = IHS_TimerTaskStart(manager->session->timers,
            LunarHIDPoll, NULL, HID_POLL_INTERVAL_MS, manager);
}
