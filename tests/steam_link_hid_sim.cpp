#include "steamlink/steam_hid.h"
#include "input/stream_input_router.h"
extern "C" {
#include "ihslib/session.h"
#include "session/session_pri.h"
#include "hid/manager.h"
#include "hid/device.h"
#include "hid/report.h"
#include "ihs_buffer.h"
#include "ihs_enumeration.h"
}
#include <cassert>
#include <iostream>
#include <vector>
#include <thread>
#include <chrono>
using namespace lunar::steamlink;

int main() {
    lunar::input::GamepadState state;
    state.a = true; state.guide = true; state.menu = true;
    state.left_stick_x = -32768; state.left_stick_y = 32767;
    state.right_stick_x = 32767; state.right_stick_y = -32768;
    state.lt = true; state.right_trigger = 32768;
    auto encoded = encodeSteamPad(state);
    assert(encoded[0] == 0 && encoded[1] == 128);
    assert(encoded[2] == 1 && encoded[3] == 128);
    assert(encoded[4] == 255 && encoded[5] == 127);
    assert(encoded[6] == 255 && encoded[7] == 127);
    assert(encoded[8] == 255 && encoded[9] == 127);
    assert(encoded[10] == 0 && encoded[11] == 64);
    assert(encoded[16] == 0x61);
    bool lunar::input::GamepadState::*buttons[] = {
        &lunar::input::GamepadState::a, &lunar::input::GamepadState::b,
        &lunar::input::GamepadState::x, &lunar::input::GamepadState::y,
        &lunar::input::GamepadState::view, &lunar::input::GamepadState::guide,
        &lunar::input::GamepadState::menu, &lunar::input::GamepadState::l3,
        &lunar::input::GamepadState::r3, &lunar::input::GamepadState::lb,
        &lunar::input::GamepadState::rb, &lunar::input::GamepadState::dpad_up,
        &lunar::input::GamepadState::dpad_down, &lunar::input::GamepadState::dpad_left,
        &lunar::input::GamepadState::dpad_right, &lunar::input::GamepadState::touchpad};
    for (int i = 0; i < 16; ++i) {
        lunar::input::GamepadState one; one.*buttons[i] = true;
        const auto r = encodeSteamPad(one);
        assert((r[16] | (unsigned(r[17]) << 8)) == (1u << i));
    }
    IHS_Init();
    uint8_t secret[32]{};
    IHS_ClientConfig config{};
    config.deviceId = 100; config.secretKey = secret; config.deviceName = "LunarNX HID simulation";
    IHS_SessionInfo info{};
    info.address.ip.family = IHS_IPAddressFamilyIPv4;
    info.address.ip.v4.data[0] = 127; info.address.ip.v4.data[3] = 1;
    info.address.port = 27031; info.sessionKeyLen = 16;
    auto* session = IHS_SessionCreate(&config, &info);
    auto shared = std::make_shared<SteamPadState>();
    auto* provider = createSteamPadProvider(shared);
    IHS_SessionHIDAddProvider(session, provider);
    auto* enumeration = provider->cls->enumerateDevices(provider);
    assert(IHS_EnumerationCount(enumeration) == 1);
    IHS_HIDDeviceInfo device_info{};
    provider->cls->deviceInfo(provider, enumeration, &device_info);
    assert(device_info.vendor_id == 0x057e);
    IHS_EnumerationFree(enumeration);
    for (int repeat = 0; repeat < 10; ++repeat) {
        auto* managed = IHS_HIDManagerOpenDevice(session->hidManager, device_info.path);
        assert(managed && shared->opened);
        auto* device = managed->device;
        assert(device->cls->requestFullReport(device) == -1);
        assert(device->cls->startInputReports(device, 0) == -1);
        IHS_HIDReportHolderSetReportLength(&managed->reportHolder, 48);
        assert(device->cls->startInputReports(device, 48) == 0);
        IHS_HIDReportHolderResetMessage(&managed->reportHolder);
        const uint8_t version[] = {9, 3};
        assert(device->cls->write(device, version, 2) == 0);
        shared->publish(state);
        IHS_HIDDeviceLock(device);
        assert(device->cls->poll(device) == 1);
        auto* message = IHS_HIDReportHolderGetMessage(&managed->reportHolder);
        assert(message && message->n_reports == 1);
        const auto bytes = protobuf_c_message_get_packed_size(&message->base);
        std::vector<uint8_t> packet(bytes);
        protobuf_c_message_pack(&message->base, packet.data());
        auto* parsed = reinterpret_cast<CHIDMessageFromRemote__DeviceInputReports__DeviceInputReport*>(protobuf_c_message_unpack(
            &chidmessage_from_remote__device_input_reports__device_input_report__descriptor,
            nullptr, packet.size(), packet.data()));
        assert(parsed && parsed->device == managed->id);
        auto report = parsed->reports[0]->full_report;
        assert(report.len == 48 && report.data[16] == 0x61 && report.data[27] == 3);
        assert(report.data[0] == 0 && report.data[1] == 128);
        protobuf_c_message_free_unpacked(&parsed->base, nullptr);
        IHS_HIDReportHolderResetMessage(&managed->reportHolder);
        assert(device->cls->poll(device) == 0);
        IHS_HIDDeviceUnlock(device);
        assert(device->cls->requestFullReport(device) == 0);
        assert(IHS_HIDReportHolderGetMessage(&managed->reportHolder) != nullptr);
        IHS_HIDReportHolderResetMessage(&managed->reportHolder);
        lunar::input::StreamInputRouter router;
        router.setOwner(lunar::input::StreamInputOwner::Ui);
        shared->publish(router.route(state));
        IHS_HIDDeviceLock(device);
        assert(device->cls->poll(device) == 1);
        message = IHS_HIDReportHolderGetMessage(&managed->reportHolder);
        report = message->reports[0]->full_report;
        for (int i = 0; i < 27; ++i) assert(report.data[i] == 0);
        IHS_HIDReportHolderResetMessage(&managed->reportHolder);
        IHS_HIDDeviceUnlock(device);
        IHS_Buffer buffer{};
        IHS_BufferInit(&buffer, 64, 128);
        uint8_t command = 4;
        assert(device->cls->getFeatureReport(device, &command, 1, &buffer, 64) == 0);
        assert(buffer.size == 21 && IHS_BufferPointer(&buffer)[3] == 44);
        assert(device->cls->getFeatureReport(device, &command, 1, &buffer, 2) == -1);
        IHS_BufferClear(&buffer, true);
        const uint8_t rumble[] = {1, 255,255, 0,128, 0xe8,3,0,0};
        for (size_t n = 0; n < 9; ++n) assert(device->cls->write(device, rumble, n) == -1);
        assert(device->cls->write(device, rumble, sizeof(rumble)) == 0);
        assert(shared->rumble_low == 65535 && shared->rumble_high == 32768 && shared->rumble_duration == 1000);
        IHS_HIDManagedDeviceClose(managed);
        IHS_HIDManagedDeviceClose(managed); // close must be idempotent
        assert(!shared->opened && shared->rumble_low == 0);
    }
    IHS_SessionDestroy(session);
    destroySteamPadProvider(provider);
    // Exercise actual timed polling concurrently with host close/reopen.
    session = IHS_SessionCreate(&config, &info);
    session->state.streamingInput = true;
    provider = createSteamPadProvider(shared);
    IHS_SessionHIDAddProvider(session, provider);
    std::atomic<bool> running{true};
    std::thread producer([&] {
        lunar::input::GamepadState changing;
        while (running.load()) {
            changing.a = !changing.a;
            shared->publish(changing);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });
    for (int i = 0; i < 100; ++i) {
        auto* slot = IHS_HIDManagerOpenDevice(session->hidManager, device_info.path);
        IHS_HIDReportHolderSetReportLength(&slot->reportHolder, 48);
        assert(slot->device->cls->startInputReports(slot->device, 48) == 0);
        std::this_thread::sleep_for(std::chrono::milliseconds(1 + i % 10));
        IHS_HIDManagedDeviceClose(slot);
    }
    running = false;
    producer.join();
    // Destruction must also close a still-open device before freeing channels.
    IHS_HIDManagerOpenDevice(session->hidManager, device_info.path);
    IHS_SessionDestroy(session);
    destroySteamPadProvider(provider);
    IHS_Quit();
    std::cout << "PASS: native HID mapping/protobuf, neutral UI release, features, rumble, explicit resync, idempotent close, 100 concurrent timer close/reopen cycles\n";
}
