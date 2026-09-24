#include "steam_hid.h"
extern "C" {
#include <ihslib/buffer.h>
#include "ihs_buffer.h"
#include "hid/device.h"
#include "hid/manager.h"
}
#include <algorithm>
#include <cstring>

namespace lunar::steamlink {
namespace {
void put16(uint8_t* p, uint16_t n) { p[0] = n; p[1] = n >> 8; }
uint16_t get16(const uint8_t* p) { return p[0] | (uint16_t(p[1]) << 8); }
uint32_t get32(const uint8_t* p) { return get16(p) | (uint32_t(get16(p + 2)) << 16); }
constexpr const char* kPath = "sdl://lunarnx/0";
struct Provider : IHS_HIDProvider { std::shared_ptr<SteamPadState> state; };
struct Device : IHS_HIDDevice {
    std::shared_ptr<SteamPadState> state;
    SteamPadReport previous{};
    bool started = false;
};
Device* dev(IHS_HIDDevice* p) { return static_cast<Device*>(p); }
SteamPadReport snapshot(Device* p) {
    std::lock_guard<std::mutex> lock(p->state->mutex);
    return p->state->report;
}
int full(IHS_HIDDevice* p) {
    IHS_HIDDeviceLock(p);
    auto* d = dev(p);
    if (p->managed->reportHolder.reportLength < 48) {
        IHS_HIDDeviceUnlock(p);
        return -1;
    }
    d->previous = snapshot(d);
    // An explicit host resync must bypass unchanged-state suppression.
    p->managed->reportHolder.lastSentLen = 0;
    IHS_HIDDeviceReportAddFull(p, d->previous.data(), d->previous.size());
    d->state->reports++;
    IHS_HIDDeviceUnlock(p);
    return 0;
}
int poll(IHS_HIDDevice* p) {
    // ihslib owns the device lock around this callback.
    auto* d = dev(p);
    if (!d->started || p->managed->reportHolder.reportLength < 48) return 0;
    const auto current = snapshot(d);
    if (current == d->previous) return 0;
    IHS_HIDDeviceReportAddFull(p, current.data(), current.size());
    d->previous = current;
    d->state->reports++;
    return 1;
}
int start(IHS_HIDDevice* p, size_t length) {
    if (length < 48) return -1;
    IHS_HIDDeviceLock(p);
    dev(p)->started = true;
    IHS_HIDDeviceUnlock(p);
    return full(p);
}
int write(IHS_HIDDevice* p, const uint8_t* data, size_t size) {
    if (!data || !size) return -1;
    auto& s = *dev(p)->state;
    std::lock_guard<std::mutex> lock(s.mutex);
    switch (data[0]) {
        case 1: // low/high frequency motors, duration in milliseconds
        case 6: // map trigger feedback to the two available Switch motors
            if (size < 9) return -1;
            s.rumble_low = get16(data + 1);
            s.rumble_high = get16(data + 3);
            s.rumble_duration = get32(data + 5);
            ++s.rumble_generation;
            return 0;
        case 9:
            if (size < 2) return -1;
            s.report[27] = data[1];
            return 0;
        case 5: return size >= 4 ? 0 : -1; // no RGB LED on the Switch
        case 8:
            if (size < 2) return -1;
            s.sensors_requested = data[1] != 0;
            return 0;
        case 10: return -1; // no DualSense effects
        case 11: return size >= 2 ? 0 : -1; // one virtual player slot
        default: return -1;
    }
}
int feature(IHS_HIDDevice*, const uint8_t* command, size_t count,
            IHS_Buffer* out, size_t length) {
    if (!command || !count || !out) return -1;
    std::array<uint8_t, 32> data{};
    data[0] = command[0];
    size_t size = 0;
    switch (command[0]) {
        case 4: // generic SDL capability reply, not a raw Nintendo HID device
            size = 21;
            data[1] = 1; // valid
            data[3] = 44; // XInputSwitchController: Switch-compatible generic input
            break;
        case 2: size = 2; data[1] = 255; break; // power unknown
        case 7:
            size = 10;
            std::memcpy(data.data() + 1, "LunarNX-0", 9);
            break;
        default: return -1;
    }
    if (length < size) return -1;
    IHS_BufferWriteMem(out, 0, data.data(), size);
    return 0;
}
int text(IHS_Buffer* out, const char* value) {
    IHS_BufferWriteMem(out, 0, reinterpret_cast<const uint8_t*>(value), std::strlen(value) + 1);
    return 0;
}
const IHS_HIDDeviceClass device_class = [] {
    IHS_HIDDeviceClass c{};
    c.alloc = [](const IHS_HIDDeviceClass* cls) -> IHS_HIDDevice* {
        auto* p = new Device{}; p->cls = cls; return p;
    };
    c.free = [](IHS_HIDDevice* p) { delete dev(p); };
    c.opened = [](IHS_HIDDevice* p) { dev(p)->state->opened = true; };
    c.close = [](IHS_HIDDevice* p) {
        auto& s = *dev(p)->state;
        s.opened = false;
        std::lock_guard<std::mutex> lock(s.mutex);
        s.sensors_requested = false;
        s.rumble_low = s.rumble_high = 0; s.rumble_duration = 0; ++s.rumble_generation;
    };
    c.write = write;
    c.read = [](IHS_HIDDevice* p, IHS_Buffer* out, size_t n, uint32_t) {
        if (n < 48) return -1;
        const auto r = snapshot(dev(p)); IHS_BufferWriteMem(out, 0, r.data(), r.size());
        return 48;
    };
    c.sendFeatureReport = [](IHS_HIDDevice*, const uint8_t*, size_t) { return -1; };
    c.getFeatureReport = feature;
    c.getVendorString = [](IHS_HIDDevice*, IHS_Buffer* out) { return text(out, "LunarNX"); };
    c.getProductString = [](IHS_HIDDevice*, IHS_Buffer* out) { return text(out, "LunarNX Switch Gamepad"); };
    c.getSerialNumberString = [](IHS_HIDDevice*, IHS_Buffer* out) { return text(out, "LunarNX-0"); };
    c.startInputReports = start;
    c.requestFullReport = full;
    c.requestDisconnect = [](IHS_HIDDevice*, int, const uint8_t*, size_t) { return 0; };
    c.poll = poll;
    return c;
}();
const IHS_HIDProviderClass provider_class = [] {
    IHS_HIDProviderClass c{};
    c.alloc = [](const IHS_HIDProviderClass* cls) -> IHS_HIDProvider* {
        auto* p = new Provider{}; p->cls = cls; return p;
    };
    c.free = [](IHS_HIDProvider* p) { delete static_cast<Provider*>(p); };
    c.supportsDevice = [](IHS_HIDProvider*, const char* path) {
        return path && std::strcmp(path, kPath) == 0;
    };
    c.openDevice = [](IHS_HIDProvider* p, const char* path) -> IHS_HIDDevice* {
        if (!path || std::strcmp(path, kPath)) return nullptr;
        auto* d = static_cast<Device*>(device_class.alloc(&device_class));
        d->state = static_cast<Provider*>(p)->state;
        return d;
    };
    c.hasChange = [](IHS_HIDProvider*) { return false; };
    c.enumerateDevices = [](IHS_HIDProvider* p) {
        return IHS_EnumerationArrayCreate(p, sizeof(IHS_HIDProvider), 1, nullptr);
    };
    c.deviceInfo = [](IHS_HIDProvider*, IHS_Enumeration*, IHS_HIDDeviceInfo* info) {
        *info = {}; info->path = kPath; info->product_string = "LunarNX Switch Gamepad";
        info->serial_number = "LunarNX-0";
        info->vendor_id = 0x057e; info->product_id = 0x2009; info->product_version = 1;
    };
    return c;
}();
} // namespace

SteamPadReport encodeSteamPad(const input::GamepadState& s) {
    SteamPadReport r{};
    // SDL axes use positive-down Y; libnx/GamepadState uses positive-up.
    const auto invert = [](int16_t v) { return int16_t(std::clamp(-int(v), -32768, 32767)); };
    const int16_t axes[] = {s.left_stick_x, invert(s.left_stick_y),
        s.right_stick_x, invert(s.right_stick_y),
        int16_t(s.lt ? 32767 : s.left_trigger / 2),
        int16_t(s.rt ? 32767 : s.right_trigger / 2)};
    for (size_t i = 0; i < 6; ++i) put16(r.data() + 2 * i, uint16_t(axes[i]));
    const bool buttons[] = {s.a,s.b,s.x,s.y,s.view,s.guide,s.menu,s.l3,s.r3,
        s.lb,s.rb,s.dpad_up,s.dpad_down,s.dpad_left,s.dpad_right,s.touchpad};
    uint16_t bits = 0;
    for (size_t i = 0; i < 16; ++i) if (buttons[i]) bits |= uint16_t(1u << i);
    put16(r.data() + 16, bits);
    return r;
}
void SteamPadState::publish(const input::GamepadState& state, const MotionSample& motion) {
    auto next = encodeSteamPad(state);
    std::lock_guard<std::mutex> lock(mutex);
    if (sensors_requested && motion.valid) {
        const auto quantize = [](float v, float range) {
            if (!std::isfinite(v)) return int16_t(0);
            return int16_t(-32768.f + (std::clamp(v/range,-1.f,1.f)+1.f)*32767.5f);
        };
        for(size_t i=0;i<3;++i) {
            put16(next.data()+28+2*i, uint16_t(quantize(motion.gyro[i],34.90659f)));
            put16(next.data()+34+2*i, uint16_t(quantize(motion.accel[i],19.6133f)));
        }
    }
    next[27] = report[27]; report = next;
}
IHS_HIDProvider* createSteamPadProvider(std::shared_ptr<SteamPadState> state) {
    auto* p = static_cast<Provider*>(provider_class.alloc(&provider_class));
    p->state = std::move(state); return p;
}
void destroySteamPadProvider(IHS_HIDProvider* p) { if (p) p->cls->free(p); }
} // namespace lunar::steamlink
