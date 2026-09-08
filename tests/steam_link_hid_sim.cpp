#include "steamlink/steam_hid.h"
#include "steamlink/stream_support.h"
#include "input/stream_input_router.h"
extern "C" {
#include "ihslib/session.h"
#include "session/session_pri.h"
#include "session/channels/ch_control.h"
#include "hid/manager.h"
#include "hid/device.h"
#include "hid/report.h"
#include "ihs_buffer.h"
#include "ihs_enumeration.h"
#include "client/client_pri.h"
#include "steamlink/ihs_streaming_support.h"
}
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cassert>
#include <iostream>
#include <vector>
#include <thread>
#include <chrono>
using namespace lunar::steamlink;

static bool negotiated = false, expected_hid = false;
static bool completed = false, complete_send_ok = true;
extern "C" bool CaptureNegotiationSend(IHS_SessionChannel*, EStreamControlMessage type,
                                      const ProtobufCMessage* message, int32_t) {
    if (type == k_EStreamControlNegotiationComplete) {
        completed = true;
        return complete_send_ok;
    }
    assert(type == k_EStreamControlNegotiationSetConfig);
    std::vector<uint8_t> bytes(protobuf_c_message_get_packed_size(message));
    protobuf_c_message_pack(message, bytes.data());
    auto* reply = cnegotiation_set_config_msg__unpack(nullptr, bytes.size(), bytes.data());
    assert(reply && reply->config && reply->streaming_client_config);
    assert(reply->config->has_enable_remote_hid);
    assert(bool(reply->config->enable_remote_hid) == expected_hid);
    assert(reply->config->selected_video_codec == k_EStreamVideoCodecH264);
    assert(reply->config->selected_audio_codec == k_EStreamAudioCodecOpus);
    assert(reply->config->n_available_video_modes == 1);
    assert(reply->config->available_video_modes[0]->width == 1280);
    assert(reply->config->available_video_modes[0]->height == 720);
    assert(reply->streaming_client_config->maximum_resolution_x == 1280);
    assert(reply->streaming_client_config->maximum_resolution_y == 720);
    cnegotiation_set_config_msg__free_unpacked(reply, nullptr);
    negotiated = true;
    return true;
}

static void testNegotiation(IHS_Session* session, bool has_provider) {
    for (bool host_hid : {false, true}) {
        expected_hid = has_provider && host_hid; negotiated = false;
        session->state.connectionState = IHS_SessionConnectionStateNegotiating;
        CNegotiationInitMsg init = CNEGOTIATION_INIT_MSG__INIT;
        EStreamAudioCodec audio = k_EStreamAudioCodecOpus;
        EStreamVideoCodec video = k_EStreamVideoCodecH264;
        init.n_supported_audio_codecs = 1; init.supported_audio_codecs = &audio;
        init.n_supported_video_codecs = 1; init.supported_video_codecs = &video;
        init.has_supports_remote_hid = true; init.supports_remote_hid = host_hid;
        std::vector<uint8_t> bytes(protobuf_c_message_get_packed_size(&init.base));
        protobuf_c_message_pack(&init.base, bytes.data());
        IHS_Buffer buffer{};
        IHS_BufferInit(&buffer, bytes.size(), bytes.size());
        IHS_BufferWriteMem(&buffer, 0, bytes.data(), bytes.size());
        IHS_SessionPacketHeader header{};
        IHS_SessionChannelControlOnNegotiation(
            IHS_SessionChannelFor(session, IHS_SessionChannelIdControl),
            k_EStreamControlNegotiationInit, &buffer, &header);
        assert(negotiated);
        IHS_BufferClear(&buffer, true);
    }
}

static void testHostConfig(const IHS_ClientConfig& config, const IHS_SessionInfo& info) {
    // Accepted, rejected, absent HID flag, missing config, input veto, send failure.
    for (int scenario=0; scenario<6; ++scenario) {
        auto* session = IHS_SessionCreate(&config, &info);
        auto* provider = createSteamPadProvider(std::make_shared<SteamPadState>());
        IHS_SessionHIDAddProvider(session, provider);
        session->state.connectionState = IHS_SessionConnectionStateNegotiating;
        CNegotiatedConfig selected = CNEGOTIATED_CONFIG__INIT;
        selected.has_enable_remote_hid = scenario != 2;
        selected.enable_remote_hid = scenario != 1;
        CStreamingClientConfig client = CSTREAMING_CLIENT_CONFIG__INIT;
        client.has_enable_input_streaming = scenario == 4;
        client.enable_input_streaming = false;
        CNegotiationSetConfigMsg message = CNEGOTIATION_SET_CONFIG_MSG__INIT;
        message.config = scenario == 3 ? nullptr : &selected;
        message.streaming_client_config = &client;
        std::vector<uint8_t> bytes(protobuf_c_message_get_packed_size(&message.base));
        protobuf_c_message_pack(&message.base, bytes.data());
        // Required message fields pack nullptr as an empty message. Use raw
        // wire bytes to truly omit required field 1 (only client config remains).
        if (scenario == 3) bytes = {0x12, 0x00};
        IHS_Buffer payload{};
        IHS_BufferInit(&payload, bytes.size(), bytes.size());
        IHS_BufferWriteMem(&payload, 0, bytes.data(), bytes.size());
        IHS_SessionPacketHeader header{};
        completed = false; complete_send_ok = scenario != 5;
        IHS_SessionChannelControlOnNegotiation(
            IHS_SessionChannelFor(session, IHS_SessionChannelIdControl),
            k_EStreamControlNegotiationSetConfig, &payload, &header);
        const bool accepted = scenario == 0 || scenario == 2;
        assert((session->state.connectionState == IHS_SessionConnectionStateConnected) == accepted);
        assert(completed == (accepted || scenario == 5));
        IHS_BufferClear(&payload, true);
        IHS_SessionInterrupt(session);
        IHS_SessionDestroy(session);
        destroySteamPadProvider(provider);
    }
    complete_send_ok = true;
    std::cout << "PASS: host final HID accept/reject/absent, missing config, input veto and send failure\n";
}

static void testSessionShutdown(const IHS_ClientConfig& config, const IHS_SessionInfo& info) {
    // Reserve an ephemeral loopback peer, never the local Steam installation.
    int peer = socket(AF_INET, SOCK_DGRAM, 0);
    assert(peer >= 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    assert(bind(peer, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
    socklen_t length = sizeof(address);
    assert(getsockname(peer, reinterpret_cast<sockaddr*>(&address), &length) == 0);
    auto local_info = info;
    local_info.address.port = ntohs(address.sin_port);
    struct Context { SessionDisconnectGate gate; std::atomic<bool> ready{false}; } context;
    IHS_StreamSessionCallbacks callbacks{};
    callbacks.initialized = [](IHS_Session*, void* p) { static_cast<Context*>(p)->ready = true; };
    callbacks.disconnected = [](IHS_Session*, void* p) { static_cast<Context*>(p)->gate.disconnected(); };
    for (int cycle = 0; cycle < 20; ++cycle) {
        context.gate.reset(); context.ready = false;
        auto* session = IHS_SessionCreate(&config, &local_info);
        IHS_SessionSetSessionCallbacks(session, &callbacks, &context);
        assert(IHS_SessionConnect(session));
        while (!context.ready) std::this_thread::yield();
        unsigned requests = 0;
        auto disconnect = [&](IHS_Session* value) {
            context.gate.request([&] { ++requests; IHS_SessionDisconnect(value); });
        };
        // Protocol errors can initiate disconnect before the owner's gate sees
        // a callback. Exercise that real path, not just duplicate owner calls.
        auto* control = IHS_SessionChannelFor(session, IHS_SessionChannelIdControl);
        IHS_SessionPacket unexpected{};
        unexpected.header.type = IHS_SessionPacketTypeUnreliable;
        control->cls->received(control, &unexpected);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        // The owner initiates exactly once even when cleanup is requested again.
        disconnect(session);
        disconnect(session);
        std::thread host;
        if (cycle != 0) {
            auto* discovery = IHS_SessionChannelFor(session, IHS_SessionChannelIdDiscovery);
            host = std::thread([discovery, cycle] {
                std::this_thread::sleep_for(std::chrono::milliseconds(cycle % 5));
                IHS_SessionPacket packet{};
                packet.header.type = IHS_SessionPacketTypeDisconnect;
                discovery->cls->received(discovery, &packet);
            });
        }
        closeSession(session, disconnect, [&](IHS_Session* value) {
            if (host.joinable()) host.join();
            IHS_SessionThreadedJoin(value);
        }, IHS_SessionDestroy);
        assert(requests == 1 && session == nullptr);
    }
    close(peer);
    std::cout << "PASS: 20 real session disconnect/join/destroy cycles, no-host timeout and host-disconnect race\n";
}

// Real timer/request/protobuf code against a loopback receiver, not Steam.
static void testStreamingCancellation(const IHS_ClientConfig& config) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    assert(fd >= 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    assert(bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
    socklen_t length = sizeof(address);
    assert(getsockname(fd, reinterpret_cast<sockaddr*>(&address), &length) == 0);
    timeval timeout{2, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    IHS_Client client{};
    IHS_BaseInit(&client.base, &config, nullptr, false);
    client.base.socket = IHS_UDPSocketOpen(false);
    client.timers = IHS_TimerCreate();
    IHS_HostInfo host{};
    host.clientId = 123;
    host.address.ip.family = IHS_IPAddressFamilyIPv4;
    host.address.ip.v4.data[0] = 127; host.address.ip.v4.data[3] = 1;
    host.address.port = ntohs(address.sin_port);
    std::atomic<unsigned> progress{0};
    IHS_ClientStreamingCallbacks callbacks{};
    callbacks.progress = [](IHS_Client*, const IHS_HostInfo*, void* p) {
        ++*static_cast<std::atomic<unsigned>*>(p);
    };
    IHS_ClientSetStreamingCallbacks(&client, &callbacks, &progress);
    IHS_StreamingRequest request{};
    uint32_t previous_id = 0;
    for (int cycle = 0; cycle < 100; ++cycle) {
        assert(IHS_ClientStreamingRequest(&client, &host, &request));
        assert(!IHS_ClientStreamingRequest(&client, &host, &request));
        uint8_t data[2048];
        const auto size = recv(fd, data, sizeof(data), 0);
        assert(size > 16);
        auto le32 = [&](size_t i) { return uint32_t(data[i]) | uint32_t(data[i+1])<<8 |
            uint32_t(data[i+2])<<16 | uint32_t(data[i+3])<<24; };
        const auto offset = 12 + le32(8);
        assert(offset + 4 <= size && offset + 4 + le32(offset) == size);
        auto* sent = cmsg_remote_device_streaming_request__unpack(nullptr, le32(offset), data+offset+4);
        assert(sent);
        const auto id = sent->request_id;
        cmsg_remote_device_streaming_request__free_unpacked(sent, nullptr);
        CMsgRemoteClientBroadcastHeader header = CMSG_REMOTE_CLIENT_BROADCAST_HEADER__INIT;
        header.has_client_id = true; header.client_id = host.clientId;
        header.msg_type = k_ERemoteDeviceStreamingResponse;
        CMsgRemoteDeviceStreamingResponse response = CMSG_REMOTE_DEVICE_STREAMING_RESPONSE__INIT;
        response.result = k_ERemoteDeviceStreamingInProgress;
        response.request_id = previous_id == id ? id + 1 : previous_id;
        const auto before = progress.load();
        IHS_ClientStreamingCallback(&client, &host.address, &header, &response.base);
        assert(progress == before); // late previous-generation response ignored
        response.request_id = id;
        IHS_ClientStreamingCallback(&client, &host.address, &header, &response.base);
        assert(progress == before + 1);
        std::thread incoming([&] {
            for (int i = 0; i < 100; ++i)
                IHS_ClientStreamingCallback(&client, &host.address, &header, &response.base);
        });
        LunarIHSStreamingCancel(&client);
        const auto canceled = progress.load();
        incoming.join();
        assert(progress == canceled);
        LunarIHSStreamingCancel(&client);
        assert(!client.taskHandles.streaming);
        previous_id = id;
    }
    IHS_TimerDestroy(client.timers);
    IHS_UDPSocketClose(client.base.socket);
    IHS_BaseDestroy(&client.base);
    close(fd);
    std::cout << "PASS: 100 loopback request/cancel/retry cycles, stale IDs, concurrent callbacks drained\n";
}

extern "C" void ExpireSteamAuthorization(IHS_Client*);
static void testAuthorization(const IHS_ClientConfig& config) {
    IHS_Client client{};
    IHS_BaseInit(&client.base, &config, nullptr, false);
    client.base.socket = IHS_UDPSocketOpen(false);
    client.timers = IHS_TimerCreate();
    IHS_HostInfo host{};
    host.clientId = 123;
    host.universe = IHS_SteamUniversePublic;
    host.address.ip.family = IHS_IPAddressFamilyIPv4;
    host.address.ip.v4.data[0] = 127; host.address.ip.v4.data[3] = 1;
    // Unbound loopback port, never send authorization to a user's Steam host.
    host.address.port = 9;
    struct Counts { std::atomic<unsigned> success{0}, timeout{0}; } counts;
    IHS_ClientAuthorizationCallbacks callbacks{};
    callbacks.success = [](IHS_Client*, const IHS_HostInfo*, uint64_t id, void* p) {
        assert(id == 456); ++static_cast<Counts*>(p)->success;
    };
    callbacks.failed = [](IHS_Client*, const IHS_HostInfo*, IHS_AuthorizationResult result, void* p) {
        assert(result == IHS_AuthorizationTimedOut); ++static_cast<Counts*>(p)->timeout;
    };
    IHS_ClientSetAuthorizationCallbacks(&client, &callbacks, &counts);
    CMsgRemoteClientBroadcastHeader header = CMSG_REMOTE_CLIENT_BROADCAST_HEADER__INIT;
    header.has_client_id = true; header.client_id = host.clientId;
    header.msg_type = k_ERemoteDeviceAuthorizationResponse;
    CMsgRemoteDeviceAuthorizationResponse response = CMSG_REMOTE_DEVICE_AUTHORIZATION_RESPONSE__INIT;
    response.result = k_ERemoteDeviceAuthorizationSuccess;
    response.has_steamid = true; response.steamid = 456;
    for (int cycle = 0; cycle < 100; ++cycle) {
        assert(IHS_ClientAuthorizationRequest(&client, &host, "1234"));
        assert(!IHS_ClientAuthorizationRequest(&client, &host, "5678"));
        const auto before = counts.success.load();
        auto other = host.address; ++other.port;
        IHS_ClientAuthorizationCallback(&client, &other, &header, &response.base);
        ++header.client_id;
        IHS_ClientAuthorizationCallback(&client, &host.address, &header, &response.base);
        --header.client_id;
        IHS_ClientAuthorizationCallback(&client, &host.address, &header, nullptr);
        assert(counts.success == before);
        std::thread incoming([&] {
            for (int i=0; i<100; ++i)
                IHS_ClientAuthorizationCallback(&client, &host.address, &header, &response.base);
        });
        IHS_ClientAuthorizationCancel(&client);
        const auto drained = counts.success.load();
        incoming.join();
        assert(counts.success == drained && drained <= before + 1);
        assert(!client.taskHandles.authorization);
        assert(IHS_ClientAuthorizationRequest(&client, &host, "5678"));
        IHS_ClientAuthorizationCallback(&client, &host.address, &header, &response.base);
        assert(counts.success == drained + 1 && !client.taskHandles.authorization);
    }
    assert(IHS_ClientAuthorizationRequest(&client, &host, "1234"));
    ExpireSteamAuthorization(&client);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!counts.timeout && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    assert(counts.timeout == 1);
    IHS_ClientAuthorizationCancel(&client); // drains completion/cleanup
    assert(!client.taskHandles.authorization);
    assert(IHS_ClientAuthorizationRequest(&client, &host, "5678"));
    IHS_ClientAuthorizationCancel(&client);
    IHS_TimerDestroy(client.timers);
    IHS_UDPSocketClose(client.base.socket);
    IHS_BaseDestroy(&client.base);
    std::cout << "PASS: authorization deadline/retry, wrong host and null replies, 100 cancel/success races drained\n";
}

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
    testAuthorization(config);
    testStreamingCancellation(config);
    IHS_SessionInfo info{};
    info.address.ip.family = IHS_IPAddressFamilyIPv4;
    info.address.ip.v4.data[0] = 127; info.address.ip.v4.data[3] = 1;
    info.address.port = 27031; info.sessionKeyLen = 16;
    testSessionShutdown(config, info);
    testHostConfig(config, info);
    auto* session = IHS_SessionCreate(&config, &info);
    testNegotiation(session, false);
    auto shared = std::make_shared<SteamPadState>();
    auto* provider = createSteamPadProvider(shared);
    IHS_SessionHIDAddProvider(session, provider);
    testNegotiation(session, true);
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
        const uint8_t sensor_on[]={8,1}, sensor_off[]={8,0};
        assert(device->cls->write(device,sensor_on,1)==-1);
        assert(device->cls->write(device,sensor_on,2)==0);
        MotionSample motion{true,{34.90659f,-34.90659f,0},{19.6133f,-19.6133f,0}};
        shared->publish({},motion);
        assert(shared->report[28]==255 && shared->report[29]==127);
        assert(shared->report[30]==0 && shared->report[31]==128);
        assert(shared->report[34]==255 && shared->report[35]==127);
        assert(shared->report[36]==0 && shared->report[37]==128);
        assert(device->cls->write(device,sensor_off,2)==0);
        shared->publish({},motion);
        for(int i=28;i<40;++i) assert(shared->report[i]==0);
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
