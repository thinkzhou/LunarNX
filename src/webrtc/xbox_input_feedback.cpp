#include "xbox_input_feedback.h"

namespace lunar::webrtc {

namespace {

constexpr uint16_t kServerMetadata = 0x10;
constexpr uint16_t kVibration = 0x80;
constexpr size_t kServerMetadataSize = 8;
constexpr size_t kVibrationSize = 11;

uint16_t readU16(const uint8_t* data) {
    return static_cast<uint16_t>(data[0]) |
           (static_cast<uint16_t>(data[1]) << 8);
}

} // namespace

bool parseXboxVibrationPacket(const uint8_t* data,
                              size_t len,
                              XboxVibrationCommand& command) {
    if (!data || len < 2) return false;

    const uint16_t report_type = readU16(data);
    if ((report_type & kVibration) == 0 ||
        (report_type & ~(kServerMetadata | kVibration)) != 0) {
        return false;
    }

    size_t offset = 2;
    if ((report_type & kServerMetadata) != 0) {
        if (len < offset + kServerMetadataSize) return false;
        offset += kServerMetadataSize;
    }
    if (len < offset + kVibrationSize) return false;

    const uint8_t rumble_type = data[offset++];
    if (rumble_type != 0) return false; // FourMotorRumble

    XboxVibrationCommand parsed;
    parsed.gamepad_index = data[offset++];
    parsed.left_motor = data[offset++] / 100.0f;
    parsed.right_motor = data[offset++] / 100.0f;
    parsed.left_trigger = data[offset++] / 100.0f;
    parsed.right_trigger = data[offset++] / 100.0f;
    parsed.duration_ms = readU16(data + offset);
    offset += 2;
    parsed.delay_ms = readU16(data + offset);
    offset += 2;
    parsed.repeat = data[offset];
    command = parsed;
    return true;
}

} // namespace lunar::webrtc
