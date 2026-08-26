#pragma once

#include <cstdint>

namespace lunar::app {

enum class VideoWatchdogAction {
    None,
    ObserveRendererRecovery,
    RecoverRenderer,
    RecoverDecoder,
    ReconnectSession,
    StopStream,
};

struct VideoWatchdogObservation {
    bool rtp_stalled = false;
    bool decode_stalled = false;
    bool present_stalled = false;
    bool recovery_due = false;
    bool renderer_recovery_pending = false;
    uint32_t renderer_recovery_attempts = 0;
    uint32_t decoder_recovery_attempts = 0;
};

inline VideoWatchdogAction decideVideoWatchdogAction(
    const VideoWatchdogObservation& observation) {
    if (observation.rtp_stalled) {
        return VideoWatchdogAction::ReconnectSession;
    }
    if (!observation.recovery_due) {
        return VideoWatchdogAction::None;
    }

    // A decode stall can also make the last presented frame age. Treat it as
    // decoder damage first; a second failed recovery requires a fresh source.
    if (observation.decode_stalled) {
        return observation.decoder_recovery_attempts == 0
            ? VideoWatchdogAction::RecoverDecoder
            : VideoWatchdogAction::ReconnectSession;
    }

    // If decode is current but presentation is not, flushing a healthy decoder
    // only turns a renderer problem into a full pipeline outage. Give the UI/GPU
    // handoff one bounded recovery, then leave the stream cleanly if it cannot
    // retire its command fences.
    if (observation.present_stalled) {
        if (observation.renderer_recovery_attempts > 0) {
            return VideoWatchdogAction::StopStream;
        }
        return observation.renderer_recovery_pending
            ? VideoWatchdogAction::ObserveRendererRecovery
            : VideoWatchdogAction::RecoverRenderer;
    }

    return VideoWatchdogAction::None;
}

} // namespace lunar::app
