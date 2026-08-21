#include "xinput_encoder.h"
#include <algorithm>
#include <cstring>
#include <chrono>
#include <cmath>

namespace lunar::input {

// Button bitmask matching XStreaming's Xbox GameStream wire format.
// Reference: XStreaming src/webrtc/Packet/index.ts:182-227
enum GamepadButton : uint16_t {
    BtnNexus         = 0x0002,  // 2
    BtnMenu          = 0x0004,  // 4
    BtnView          = 0x0008,  // 8
    BtnA             = 0x0010,  // 16
    BtnB             = 0x0020,  // 32
    BtnX             = 0x0040,  // 64
    BtnY             = 0x0080,  // 128
    BtnDPadUp        = 0x0100,  // 256
    BtnDPadDown      = 0x0200,  // 512
    BtnDPadLeft      = 0x0400,  // 1024
    BtnDPadRight     = 0x0800,  // 2048
    BtnLeftShoulder  = 0x1000,  // 4096
    BtnRightShoulder = 0x2000,  // 8192
    BtnLeftThumb     = 0x4000,  // 16384
    BtnRightThumb    = 0x8000,  // 32768
};

// PhysicalPhysicality is a separate Xbox wire bitfield, not ButtonMask.
enum GamepadPhysicality : uint32_t {
    PhysDPadUp        = 0x00000001,
    PhysDPadDown      = 0x00000002,
    PhysDPadLeft      = 0x00000004,
    PhysDPadRight     = 0x00000008,
    PhysMenu          = 0x00000010,
    PhysView          = 0x00000020,
    PhysLeftThumb     = 0x00000040,
    PhysRightThumb    = 0x00000080,
    PhysLeftShoulder  = 0x00000100,
    PhysRightShoulder = 0x00000200,
    PhysNexus         = 0x00000400,
    PhysA             = 0x00001000,
    PhysB             = 0x00002000,
    PhysX             = 0x00004000,
    PhysY             = 0x00008000,
    PhysLeftTrigger   = 0x00010000,
    PhysRightTrigger  = 0x00020000,
    PhysLeftThumbX    = 0x00040000,
    PhysLeftThumbY    = 0x00080000,
    PhysRightThumbX   = 0x00100000,
    PhysRightThumbY   = 0x00200000,
};

XInputEncoder::XInputEncoder() = default;
XInputEncoder::~XInputEncoder() = default;

static void write_timestamp(std::vector<uint8_t>& buf, size_t& off) {
    auto now = std::chrono::steady_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()).count();
    double ts = static_cast<double>(ms);
    uint64_t ts_bits;
    std::memcpy(&ts_bits, &ts, sizeof(ts_bits));
    for (int i = 0; i < 8; i++) {
        buf[off++] = static_cast<uint8_t>((ts_bits >> (i * 8)) & 0xFF);
    }
}

static int16_t norm_axis(int16_t val) {
    if (val > 32767) return 32767;
    if (val < -32767) return -32767;
    return val;
}

std::vector<uint8_t> XInputEncoder::encode(const GamepadState& state) const {
    return encodeFrames({state});
}

std::vector<uint8_t> XInputEncoder::encodeFrames(
    const std::vector<GamepadState>& states) const {
    // Xbox GameStream Input wire format (ref: XStreaming Packet/index.ts)
    //
    // Packet header (14 bytes):
    //   [0-1]   ReportType (uint16 LE)
    //   [2-5]   Sequence number (uint32 LE)
    //   [6-13]  Timestamp (float64 LE)
    //
    // Gamepad section:
    //   [14]    Frame count (uint8)
    //   [15-37] Gamepad frame (23 bytes):
    //     [0]    GamepadIndex (uint8)
    //     [1-2]  ButtonMask (uint16 LE)
    //     [3-4]  LeftThumbXAxis (int16 LE)
    //     [5-6]  LeftThumbYAxis (int16 LE, positive up)
    //     [7-8]  RightThumbXAxis (int16 LE)
    //     [9-10] RightThumbYAxis (int16 LE, positive up)
    //     [11-12] LeftTrigger (uint16 LE)
    //     [13-14] RightTrigger (uint16 LE)
    //     [15-18] PhysicalPhysicality (uint32 LE)
    //     [19-22] VirtualPhysicality (uint32 LE)

    constexpr int HEADER_SIZE = 14;
    constexpr int FRAME_SIZE = 23;
    if (states.empty()) return {};
    const size_t frame_count = std::min(states.size(), kMaxGamepadFrames);
    const size_t total_size = HEADER_SIZE + 1 + FRAME_SIZE * frame_count;

    std::vector<uint8_t> buf(total_size, 0);
    size_t off = 0;

    // --- Header ---
    buf[off++] = 2; buf[off++] = 0;  // ReportType: Gamepad

    off += sizeof(uint32_t);  // Sequence is stamped immediately before send.

    write_timestamp(buf, off);

    // --- Gamepad section ---
    buf[off++] = static_cast<uint8_t>(frame_count);
    for (size_t frame = 0; frame < frame_count; ++frame) {
        const auto& state = states[frame];
        buf[off++] = 0;  // GamepadIndex: 0 (player 1)

        // ButtonMask (ref: XStreaming Packet/index.ts:182-228)
        uint16_t buttons = 0;
        if (state.a) buttons |= BtnA;
        if (state.b) buttons |= BtnB;
        if (state.x) buttons |= BtnX;
        if (state.y) buttons |= BtnY;
        if (state.dpad_up)    buttons |= BtnDPadUp;
        if (state.dpad_down)  buttons |= BtnDPadDown;
        if (state.dpad_left)  buttons |= BtnDPadLeft;
        if (state.dpad_right) buttons |= BtnDPadRight;
        if (state.lb) buttons |= BtnLeftShoulder;
        if (state.rb) buttons |= BtnRightShoulder;
        if (state.l3) buttons |= BtnLeftThumb;
        if (state.r3) buttons |= BtnRightThumb;
        if (state.view)  buttons |= BtnView;
        if (state.menu)  buttons |= BtnMenu;
        if (state.guide) buttons |= BtnNexus;

        buf[off++] = static_cast<uint8_t>(buttons & 0xFF);
        buf[off++] = static_cast<uint8_t>((buttons >> 8) & 0xFF);

        // libnx already reports positive Y when the stick is pushed upward.
        const int16_t lx = norm_axis(state.left_stick_x);
        const int16_t ly = norm_axis(state.left_stick_y);
        buf[off++] = static_cast<uint8_t>(lx & 0xFF);
        buf[off++] = static_cast<uint8_t>((lx >> 8) & 0xFF);
        buf[off++] = static_cast<uint8_t>(ly & 0xFF);
        buf[off++] = static_cast<uint8_t>((ly >> 8) & 0xFF);

        const int16_t rx = norm_axis(state.right_stick_x);
        const int16_t ry = norm_axis(state.right_stick_y);
        buf[off++] = static_cast<uint8_t>(rx & 0xFF);
        buf[off++] = static_cast<uint8_t>((rx >> 8) & 0xFF);
        buf[off++] = static_cast<uint8_t>(ry & 0xFF);
        buf[off++] = static_cast<uint8_t>((ry >> 8) & 0xFF);

        // Triggers
        buf[off++] = static_cast<uint8_t>(state.left_trigger & 0xFF);
        buf[off++] = static_cast<uint8_t>((state.left_trigger >> 8) & 0xFF);
        buf[off++] = static_cast<uint8_t>(state.right_trigger & 0xFF);
        buf[off++] = static_cast<uint8_t>((state.right_trigger >> 8) & 0xFF);

        uint32_t physicality = 0;
        if (state.dpad_up) physicality |= PhysDPadUp;
        if (state.dpad_down) physicality |= PhysDPadDown;
        if (state.dpad_left) physicality |= PhysDPadLeft;
        if (state.dpad_right) physicality |= PhysDPadRight;
        if (state.menu) physicality |= PhysMenu;
        if (state.view) physicality |= PhysView;
        if (state.l3) physicality |= PhysLeftThumb;
        if (state.r3) physicality |= PhysRightThumb;
        if (state.lb) physicality |= PhysLeftShoulder;
        if (state.rb) physicality |= PhysRightShoulder;
        if (state.guide) physicality |= PhysNexus;
        if (state.a) physicality |= PhysA;
        if (state.b) physicality |= PhysB;
        if (state.x) physicality |= PhysX;
        if (state.y) physicality |= PhysY;
        constexpr int16_t DEADZONE = 2000;
        if (std::abs(state.left_stick_x) > DEADZONE ||
            std::abs(state.left_stick_y) > DEADZONE) {
            physicality |= (PhysLeftThumbX | PhysLeftThumbY);
        }
        if (std::abs(state.right_stick_x) > DEADZONE ||
            std::abs(state.right_stick_y) > DEADZONE) {
            physicality |= (PhysRightThumbX | PhysRightThumbY);
        }
        if (state.left_trigger > 0) physicality |= PhysLeftTrigger;
        if (state.right_trigger > 0) physicality |= PhysRightTrigger;

        buf[off++] = static_cast<uint8_t>(physicality & 0xFF);
        buf[off++] = static_cast<uint8_t>((physicality >> 8) & 0xFF);
        buf[off++] = static_cast<uint8_t>((physicality >> 16) & 0xFF);
        buf[off++] = static_cast<uint8_t>((physicality >> 24) & 0xFF);

        // VirtualPhysicality: 0
        buf[off++] = 0;
        buf[off++] = 0;
        buf[off++] = 0;
        buf[off++] = 0;
    }

    return buf;
}

std::vector<uint8_t> XInputEncoder::encodeMetadata(uint8_t max_touch_points) const {
    constexpr int TOTAL_SIZE = 15;
    std::vector<uint8_t> buf(TOTAL_SIZE, 0);
    size_t off = 0;

    // ReportType: ClientMetadata
    buf[off++] = 8;
    buf[off++] = 0;

    off += sizeof(uint32_t);  // Sequence is stamped immediately before send.

    write_timestamp(buf, off);
    buf[off++] = max_touch_points;
    return buf;
}

bool XInputEncoder::stampSequence(uint8_t* data,
                                  size_t len,
                                  uint32_t sequence) {
    if (!data || len < 6) return false;
    data[2] = static_cast<uint8_t>(sequence);
    data[3] = static_cast<uint8_t>(sequence >> 8);
    data[4] = static_cast<uint8_t>(sequence >> 16);
    data[5] = static_cast<uint8_t>(sequence >> 24);
    return true;
}

} // namespace lunar::input
