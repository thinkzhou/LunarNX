#pragma once

#include <cstdint>

namespace lunar::stream {

enum class ResolutionFrameDecision {
    Present,
    HoldStartup,
    BeginTransition,
    HoldTransition,
    KeepCurrent,
};

class VideoResolutionTransition {
public:
    static constexpr uint64_t StartupHoldMs = 300;

    VideoResolutionTransition() = default;
    VideoResolutionTransition(int target_width, int target_height) {
        configure(target_width, target_height);
    }

    void configure(int target_width, int target_height) {
        target_width_ = target_width;
        target_height_ = target_height;
        reset();
    }

    void reset() {
        active_width_ = 0;
        active_height_ = 0;
        candidate_width_ = 0;
        candidate_height_ = 0;
        startup_deadline_ms_ = 0;
        startup_candidate_ = false;
        transitioning_ = false;
    }

    ResolutionFrameDecision observeFrame(int width, int height,
                                          uint64_t now_ms) {
        if (width <= 0 || height <= 0) {
            return ResolutionFrameDecision::KeepCurrent;
        }

        if (active_width_ == 0 || active_height_ == 0) {
            if (width == target_width_ && height == target_height_) {
                active_width_ = width;
                active_height_ = height;
                clearCandidate();
                return ResolutionFrameDecision::Present;
            }

            candidate_width_ = width;
            candidate_height_ = height;
            if (!startup_candidate_) {
                startup_candidate_ = true;
                startup_deadline_ms_ = now_ms + StartupHoldMs;
            }
            return ResolutionFrameDecision::HoldStartup;
        }

        if (transitioning_) {
            if (width == active_width_ && height == active_height_) {
                return ResolutionFrameDecision::KeepCurrent;
            }
            candidate_width_ = width;
            candidate_height_ = height;
            return ResolutionFrameDecision::HoldTransition;
        }

        if (width == active_width_ && height == active_height_) {
            return ResolutionFrameDecision::Present;
        }

        candidate_width_ = width;
        candidate_height_ = height;
        transitioning_ = true;
        return ResolutionFrameDecision::BeginTransition;
    }

    bool startupCandidateReady(uint64_t now_ms) const {
        return startup_candidate_ && now_ms >= startup_deadline_ms_;
    }

    void promoteStartupCandidate() {
        if (!startup_candidate_) return;
        active_width_ = candidate_width_;
        active_height_ = candidate_height_;
        clearCandidate();
    }

    void completeTransition() {
        if (!transitioning_) return;
        active_width_ = candidate_width_;
        active_height_ = candidate_height_;
        clearCandidate();
    }

    bool hasStartupCandidate() const { return startup_candidate_; }
    bool isTransitioning() const { return transitioning_; }
    int activeWidth() const { return active_width_; }
    int activeHeight() const { return active_height_; }
    int targetWidth() const { return target_width_; }
    int targetHeight() const { return target_height_; }
    int candidateWidth() const { return candidate_width_; }
    int candidateHeight() const { return candidate_height_; }

private:
    void clearCandidate() {
        candidate_width_ = 0;
        candidate_height_ = 0;
        startup_deadline_ms_ = 0;
        startup_candidate_ = false;
        transitioning_ = false;
    }

    int target_width_ = 0;
    int target_height_ = 0;
    int active_width_ = 0;
    int active_height_ = 0;
    int candidate_width_ = 0;
    int candidate_height_ = 0;
    uint64_t startup_deadline_ms_ = 0;
    bool startup_candidate_ = false;
    bool transitioning_ = false;
};

} // namespace lunar::stream
