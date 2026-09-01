#include "app/video_watchdog_policy.h"

#include <cassert>
#include <iostream>

namespace {

using lunar::app::VideoWatchdogAction;
using lunar::app::VideoWatchdogObservation;
using lunar::app::decideVideoWatchdogAction;

void presentOnlyStallUsesRendererRecovery() {
    VideoWatchdogObservation observation;
    observation.present_stalled = true;
    observation.recovery_due = true;

    assert(decideVideoWatchdogAction(observation) ==
           VideoWatchdogAction::RecoverRenderer);
}

void anExistingRendererRecoveryGetsOneBoundedGracePeriod() {
    VideoWatchdogObservation observation;
    observation.present_stalled = true;
    observation.recovery_due = true;
    observation.renderer_recovery_pending = true;

    assert(decideVideoWatchdogAction(observation) ==
           VideoWatchdogAction::ObserveRendererRecovery);

    observation.renderer_recovery_attempts = 1;
    assert(decideVideoWatchdogAction(observation) ==
           VideoWatchdogAction::StopStream);
}

void intentionalPresentationSuspensionDoesNotStopTheStream() {
    VideoWatchdogObservation observation;
    observation.presentation_suspended = true;
    observation.present_stalled = true;
    observation.recovery_due = true;
    observation.renderer_recovery_attempts = 1;

    assert(decideVideoWatchdogAction(observation) ==
           VideoWatchdogAction::None);

    observation.rtp_stalled = true;
    assert(decideVideoWatchdogAction(observation) ==
           VideoWatchdogAction::ReconnectSession);

    observation.rtp_stalled = false;
    observation.decode_stalled = true;
    assert(decideVideoWatchdogAction(observation) ==
           VideoWatchdogAction::RecoverDecoder);
}

void decodeRecoveryEscalatesToFreshSession() {
    VideoWatchdogObservation observation;
    observation.decode_stalled = true;
    observation.present_stalled = true;
    observation.recovery_due = true;

    assert(decideVideoWatchdogAction(observation) ==
           VideoWatchdogAction::RecoverDecoder);

    observation.decoder_recovery_attempts = 1;
    assert(decideVideoWatchdogAction(observation) ==
           VideoWatchdogAction::ReconnectSession);
}

void rtpLivenessAlwaysWins() {
    VideoWatchdogObservation observation;
    observation.rtp_stalled = true;
    observation.decode_stalled = true;
    observation.present_stalled = true;

    assert(decideVideoWatchdogAction(observation) ==
           VideoWatchdogAction::ReconnectSession);
}

void cooldownSuppressesDuplicateRecovery() {
    VideoWatchdogObservation observation;
    observation.present_stalled = true;
    observation.recovery_due = false;

    assert(decideVideoWatchdogAction(observation) ==
           VideoWatchdogAction::None);
}

} // namespace

int main() {
    presentOnlyStallUsesRendererRecovery();
    anExistingRendererRecoveryGetsOneBoundedGracePeriod();
    intentionalPresentationSuspensionDoesNotStopTheStream();
    decodeRecoveryEscalatesToFreshSession();
    rtpLivenessAlwaysWins();
    cooldownSuppressesDuplicateRecovery();
    std::cout << "Video watchdog policy tests passed\n";
    return 0;
}
