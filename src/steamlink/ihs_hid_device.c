// Compile the pinned LGPL ihslib source with a synchronized close override.
// Keep the submodule pristine; these overrides are part of LunarNX's source distribution.
#define IHS_HIDManagedDeviceClose IHS_OriginalHIDManagedDeviceClose
#include "../../vendor/ihslib/src/hid/device.c"
#undef IHS_HIDManagedDeviceClose

void IHS_HIDManagedDeviceClose(IHS_HIDManagedDevice *managed) {
    IHS_MutexLock(managed->lock);
    if (managed->closed) {
        IHS_MutexUnlock(managed->lock);
        return;
    }
    IHS_HIDDevice *device = managed->device;
    device->cls->close(device);
    IHS_HIDManagerRemoveClosedDevice(managed->manager, managed);
    managed->device = NULL;
    device->managed = NULL;
    device->cls->free(device);
    IHS_MutexUnlock(managed->lock);
    IHS_HIDManagerNotifyDeviceClosed(managed->manager, managed);
}
