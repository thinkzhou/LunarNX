#pragma once

#include <cstdint>

namespace lunar::webrtc {

class RtpClockMapper {
public:
    explicit RtpClockMapper(uint32_t clock_rate) : clock_rate_(clock_rate) {}

    void reset() {
        anchored_ = false;
        anchor_rtp_ = 0;
        anchor_ns_ = 0;
    }

    uint64_t map(uint32_t rtp_timestamp, uint64_t arrival_ns) {
        if (!anchored_ || clock_rate_ == 0) {
            anchored_ = true;
            anchor_rtp_ = rtp_timestamp;
            anchor_ns_ = arrival_ns;
            return arrival_ns;
        }

        const int64_t delta = static_cast<int32_t>(rtp_timestamp - anchor_rtp_);
        const int64_t offset_ns =
            (delta * 1'000'000'000LL) / static_cast<int64_t>(clock_rate_);
        const int64_t predicted_ns =
            static_cast<int64_t>(anchor_ns_) + offset_ns;
        const int64_t arrival_error_ns =
            static_cast<int64_t>(arrival_ns) - predicted_ns;
        if (arrival_error_ns > 2'000'000'000LL ||
            arrival_error_ns < -2'000'000'000LL) {
            // A sender can restart its RTP clock while retaining the same
            // SSRC. Re-anchor at the first packet after that discontinuity so
            // AV sync does not classify every subsequent frame as stale.
            anchor_rtp_ = rtp_timestamp;
            anchor_ns_ = arrival_ns;
            return arrival_ns;
        }
        if (offset_ns < 0 && static_cast<uint64_t>(-offset_ns) > anchor_ns_) {
            return 0;
        }
        return static_cast<uint64_t>(predicted_ns);
    }

private:
    uint32_t clock_rate_ = 0;
    bool anchored_ = false;
    uint32_t anchor_rtp_ = 0;
    uint64_t anchor_ns_ = 0;
};

} // namespace lunar::webrtc
