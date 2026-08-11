#include "stream/video_resolution_transition.h"

#include <cassert>
#include <iostream>

using lunar::stream::ResolutionFrameDecision;
using lunar::stream::VideoResolutionTransition;

namespace {

void test_target_first_frame_has_no_startup_delay() {
    VideoResolutionTransition transition(1920, 1080);

    assert(transition.observeFrame(1920, 1080, 1000) ==
           ResolutionFrameDecision::Present);
    assert(transition.activeWidth() == 1920);
    assert(transition.activeHeight() == 1080);
    assert(!transition.hasStartupCandidate());
}

void test_non_target_startup_frame_waits_for_target() {
    VideoResolutionTransition transition(1920, 1080);

    assert(transition.observeFrame(2560, 1440, 1000) ==
           ResolutionFrameDecision::HoldStartup);
    assert(!transition.startupCandidateReady(1299));
    assert(transition.observeFrame(1920, 1080, 1180) ==
           ResolutionFrameDecision::Present);
    assert(!transition.hasStartupCandidate());
    assert(transition.activeWidth() == 1920);
    assert(transition.activeHeight() == 1080);
}

void test_persistent_non_target_stream_is_presented_after_deadline() {
    VideoResolutionTransition transition(1920, 1080);

    assert(transition.observeFrame(2560, 1440, 1000) ==
           ResolutionFrameDecision::HoldStartup);
    assert(!transition.startupCandidateReady(1299));
    assert(transition.startupCandidateReady(1300));
    transition.promoteStartupCandidate();
    assert(transition.activeWidth() == 2560);
    assert(transition.activeHeight() == 1440);
}

void test_non_target_xbox_startup_frame_is_presented_immediately() {
    VideoResolutionTransition transition(1920, 1080, false);

    assert(transition.observeFrame(1280, 720, 1000) ==
           ResolutionFrameDecision::Present);
    assert(transition.activeWidth() == 1280);
    assert(transition.activeHeight() == 720);
    assert(!transition.hasStartupCandidate());

    assert(transition.observeFrame(1920, 1080, 1010) ==
           ResolutionFrameDecision::BeginTransition);
    assert(transition.isTransitioning());
}

void test_runtime_change_drains_once_and_tracks_latest_candidate() {
    VideoResolutionTransition transition(1920, 1080);
    assert(transition.observeFrame(2560, 1440, 0) ==
           ResolutionFrameDecision::HoldStartup);
    transition.promoteStartupCandidate();

    assert(transition.observeFrame(1920, 1080, 1000) ==
           ResolutionFrameDecision::BeginTransition);
    assert(transition.isTransitioning());
    assert(transition.observeFrame(1920, 1080, 1010) ==
           ResolutionFrameDecision::HoldTransition);
    assert(transition.observeFrame(1600, 900, 1015) ==
           ResolutionFrameDecision::HoldTransition);
    assert(transition.candidateWidth() == 1600);
    assert(transition.candidateHeight() == 900);
    assert(transition.observeFrame(1920, 1080, 1018) ==
           ResolutionFrameDecision::HoldTransition);
    assert(transition.observeFrame(2560, 1440, 1020) ==
           ResolutionFrameDecision::KeepCurrent);

    transition.completeTransition();
    assert(!transition.isTransitioning());
    assert(transition.activeWidth() == 1920);
    assert(transition.activeHeight() == 1080);
    assert(transition.observeFrame(1920, 1080, 1030) ==
           ResolutionFrameDecision::Present);
}

void test_reset_requires_startup_selection_again() {
    VideoResolutionTransition transition(1920, 1080);
    assert(transition.observeFrame(1920, 1080, 1000) ==
           ResolutionFrameDecision::Present);
    transition.reset();
    assert(transition.activeWidth() == 0);
    assert(transition.observeFrame(2560, 1440, 2000) ==
           ResolutionFrameDecision::HoldStartup);
}

} // namespace

int main() {
    test_target_first_frame_has_no_startup_delay();
    test_non_target_startup_frame_waits_for_target();
    test_persistent_non_target_stream_is_presented_after_deadline();
    test_non_target_xbox_startup_frame_is_presented_immediately();
    test_runtime_change_drains_once_and_tracks_latest_candidate();
    test_reset_requires_startup_selection_again();
    std::cout << "Video resolution transition tests passed\n";
    return 0;
}
