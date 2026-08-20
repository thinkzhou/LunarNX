#include "webrtc/rtp_clock_mapper.h"

#include <cassert>
#include <cstdint>

using lunar::webrtc::RtpClockMapper;

int main() {
    RtpClockMapper video(90000);
    assert(video.map(9000, 1'000'000'000ULL) == 1'000'000'000ULL);
    assert(video.map(18000, 1'500'000'000ULL) == 1'100'000'000ULL);

    RtpClockMapper audio(48000);
    assert(audio.map(4800, 2'000'000'000ULL) == 2'000'000'000ULL);
    assert(audio.map(5760, 2'500'000'000ULL) == 2'020'000'000ULL);

    RtpClockMapper wrapping(48000);
    assert(wrapping.map(0xfffffff0U, 3'000'000'000ULL) == 3'000'000'000ULL);
    assert(wrapping.map(0x00000020U, 3'100'000'000ULL) == 3'001'000'000ULL);

    video.reset();
    assert(video.map(1234, 4'000'000'000ULL) == 4'000'000'000ULL);
    // A sender restart can retain its SSRC while resetting RTP timestamps.
    // The mapper must re-anchor instead of returning timestamps several
    // seconds behind the current arrival time.
    assert(video.map(100, 7'000'000'000ULL) == 7'000'000'000ULL);
    assert(video.map(9100, 7'100'000'000ULL) == 7'100'000'000ULL);
    return 0;
}
