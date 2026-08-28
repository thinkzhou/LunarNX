#pragma once

#include "../stream/realtime_latency_policy.h"
#include "../webrtc/network_path_estimator.h"

namespace lunar::app {

enum class XboxLatencyMode : uint8_t {
    Realtime,
    Balanced,
    Recovery,
};

inline const char* xboxLatencyModeName(XboxLatencyMode mode) {
    switch (mode) {
        case XboxLatencyMode::Realtime: return "realtime";
        case XboxLatencyMode::Balanced: return "balanced";
        case XboxLatencyMode::Recovery: return "recovery";
    }
    return "unknown";
}

struct XboxLatencyState {
    XboxLatencyMode mode = XboxLatencyMode::Balanced;
    stream::VideoPresentationMode video_presentation =
        stream::VideoPresentationMode::BufferedFifo;
    stream::VideoDecodeCatchUpMode video_decode_catch_up =
        stream::VideoDecodeCatchUpMode::Resilient;
    stream::AudioLatencyMode audio_latency =
        stream::AudioLatencyMode::Balanced;
};

class XboxLatencyController {
public:
    explicit XboxLatencyController(bool cloud) { reset(cloud); }

    void reset(bool cloud) {
        cloud_ = cloud;
        mode_ = cloud ? XboxLatencyMode::Balanced
                      : XboxLatencyMode::Realtime;
        clean_windows_ = 0;
        fair_windows_ = 0;
        last_sequence_ = 0;
    }

    XboxLatencyState state() const { return makeState(); }

    XboxLatencyState observe(const webrtc::NetworkPathEstimate& path,
                             bool recovery_pending) {
        if (!cloud_) {
            if (recovery_pending ||
                (path.valid && path.quality ==
                    webrtc::NetworkPathQuality::Poor)) {
                mode_ = XboxLatencyMode::Balanced;
            } else if (!path.valid || path.quality ==
                       webrtc::NetworkPathQuality::Good) {
                mode_ = XboxLatencyMode::Realtime;
            }
            return makeState();
        }

        const bool poor = path.valid &&
            (path.quality == webrtc::NetworkPathQuality::Poor ||
             path.observed_quality == webrtc::NetworkPathQuality::Poor);
        if (recovery_pending || poor) {
            mode_ = XboxLatencyMode::Recovery;
            clean_windows_ = 0;
            fair_windows_ = 0;
            return makeState();
        }

        // Recovery is an asynchronous media event and must bypass the
        // one-second network-window de-duplication below. Leaving Recovery,
        // however, still requires distinct clean estimator windows.
        if (path.valid && path.sequence != 0) {
            if (path.sequence == last_sequence_) return makeState();
            last_sequence_ = path.sequence;
        }

        const bool good = path.valid &&
            path.quality == webrtc::NetworkPathQuality::Good &&
            path.observed_quality == webrtc::NetworkPathQuality::Good;
        const bool fair = path.valid &&
            (path.quality == webrtc::NetworkPathQuality::Fair ||
             path.observed_quality == webrtc::NetworkPathQuality::Fair);

        if (mode_ == XboxLatencyMode::Recovery) {
            if (good && ++clean_windows_ >= kRecoveryCleanWindows) {
                mode_ = XboxLatencyMode::Balanced;
                clean_windows_ = 0;
            } else if (!good) {
                clean_windows_ = 0;
            }
            return makeState();
        }

        if (mode_ == XboxLatencyMode::Balanced) {
            if (good && ++clean_windows_ >= kRealtimeCleanWindows) {
                mode_ = XboxLatencyMode::Realtime;
                clean_windows_ = 0;
            } else if (!good) {
                clean_windows_ = 0;
            }
            return makeState();
        }

        if (fair) {
            if (++fair_windows_ >= kFairWindowsToBalance) {
                mode_ = XboxLatencyMode::Balanced;
                fair_windows_ = 0;
            }
        } else {
            fair_windows_ = 0;
        }
        return makeState();
    }

private:
    static constexpr uint32_t kRecoveryCleanWindows = 5;
    static constexpr uint32_t kRealtimeCleanWindows = 8;
    static constexpr uint32_t kFairWindowsToBalance = 2;

    XboxLatencyState makeState() const {
        XboxLatencyState result;
        result.mode = mode_;
        if (!cloud_) {
            result.video_presentation =
                mode_ == XboxLatencyMode::Realtime
                    ? stream::VideoPresentationMode::RealtimeAdaptive
                    : stream::VideoPresentationMode::BufferedFifo;
            result.video_decode_catch_up =
                stream::VideoDecodeCatchUpMode::Realtime;
            result.audio_latency =
                mode_ == XboxLatencyMode::Realtime
                    ? stream::AudioLatencyMode::Realtime
                    : stream::AudioLatencyMode::Balanced;
            return result;
        }
        result.video_presentation =
            mode_ == XboxLatencyMode::Realtime
                ? stream::VideoPresentationMode::RealtimeAdaptive
                : stream::VideoPresentationMode::BufferedFifo;
        result.video_decode_catch_up =
            mode_ == XboxLatencyMode::Realtime
                ? stream::VideoDecodeCatchUpMode::Realtime
                : stream::VideoDecodeCatchUpMode::Resilient;
        switch (mode_) {
            case XboxLatencyMode::Realtime:
                result.audio_latency = stream::AudioLatencyMode::Realtime;
                break;
            case XboxLatencyMode::Balanced:
                result.audio_latency = stream::AudioLatencyMode::Balanced;
                break;
            case XboxLatencyMode::Recovery:
                result.audio_latency = stream::AudioLatencyMode::Resilient;
                break;
        }
        return result;
    }

    bool cloud_ = false;
    XboxLatencyMode mode_ = XboxLatencyMode::Realtime;
    uint32_t clean_windows_ = 0;
    uint32_t fair_windows_ = 0;
    uint64_t last_sequence_ = 0;
};

inline stream::VideoPresentationMode xboxVideoPresentationMode(
    bool cloud,
    const webrtc::NetworkPathEstimate& path) {
    if (cloud) return stream::VideoPresentationMode::BufferedFifo;
    if (path.valid && path.quality != webrtc::NetworkPathQuality::Good) {
        return stream::VideoPresentationMode::BufferedFifo;
    }
    return stream::VideoPresentationMode::RealtimeAdaptive;
}

inline stream::AudioLatencyMode xboxAudioLatencyMode(bool cloud) {
    return cloud ? stream::AudioLatencyMode::Balanced
                 : stream::AudioLatencyMode::Realtime;
}

inline stream::VideoDecodeCatchUpMode xboxVideoDecodeCatchUpMode(bool cloud) {
    return cloud ? stream::VideoDecodeCatchUpMode::Resilient
                 : stream::VideoDecodeCatchUpMode::Realtime;
}

} // namespace lunar::app
