#include "app/xbox_latency_policy.h"
#include "app/video_recovery_request_policy.h"
#include "stream/realtime_latency_policy.h"

#include <cassert>
#include <iostream>

int main() {
    using namespace lunar;

    assert(stream::stalePresentationFramesToDrop(
               stream::VideoPresentationMode::BufferedFifo, 2, 30'000) == 0);
    assert(stream::stalePresentationFramesToDrop(
               stream::VideoPresentationMode::RealtimeAdaptive, 2,
               24'999) == 0);
    assert(stream::stalePresentationFramesToDrop(
               stream::VideoPresentationMode::RealtimeAdaptive, 2,
               25'000) == 1);

    auto catch_up = stream::videoDecodeCatchUpDecision(
        stream::VideoDecodeCatchUpMode::Realtime,
        false,
        2,
        8'000);
    assert(catch_up.active);
    assert(catch_up.suppress_output);
    catch_up = stream::videoDecodeCatchUpDecision(
        stream::VideoDecodeCatchUpMode::Realtime,
        catch_up.active,
        1,
        18'000);
    assert(catch_up.active);
    assert(catch_up.suppress_output);
    catch_up = stream::videoDecodeCatchUpDecision(
        stream::VideoDecodeCatchUpMode::Realtime,
        catch_up.active,
        0,
        20'000);
    assert(!catch_up.active);
    assert(!catch_up.suppress_output);

    const auto small_home_backlog = stream::videoDecodeCatchUpDecision(
        stream::VideoDecodeCatchUpMode::Realtime,
        false,
        1,
        19'999);
    assert(!small_home_backlog.active);
    assert(!small_home_backlog.suppress_output);
    const auto stale_home_backlog = stream::videoDecodeCatchUpDecision(
        stream::VideoDecodeCatchUpMode::Realtime,
        false,
        1,
        20'000);
    assert(stale_home_backlog.active);
    assert(stale_home_backlog.suppress_output);

    const auto small_cloud_backlog = stream::videoDecodeCatchUpDecision(
        stream::VideoDecodeCatchUpMode::Resilient,
        false,
        2,
        59'999);
    assert(!small_cloud_backlog.active);
    assert(!small_cloud_backlog.suppress_output);
    const auto stale_cloud_backlog = stream::videoDecodeCatchUpDecision(
        stream::VideoDecodeCatchUpMode::Resilient,
        false,
        2,
        60'000);
    assert(stale_cloud_backlog.active);
    assert(stale_cloud_backlog.suppress_output);

    const auto realtime = stream::audioBufferConfig(
        stream::AudioLatencyMode::Realtime);
    const auto balanced = stream::audioBufferConfig(
        stream::AudioLatencyMode::Balanced);
    const auto resilient = stream::audioBufferConfig(
        stream::AudioLatencyMode::Resilient);
    assert(realtime.audren_frames_per_buffer == 4);
    assert(realtime.buffer_count == 3);
    assert(realtime.max_writer_wait_frames == 4);
    assert(stream::audioBufferCapacityMs(
               stream::AudioLatencyMode::Realtime) == 60);
    assert(balanced.audren_frames_per_buffer == 4);
    assert(balanced.buffer_count == 4);
    assert(balanced.max_writer_wait_frames == 4);
    assert(stream::audioBufferCapacityMs(
               stream::AudioLatencyMode::Balanced) == 80);
    assert(resilient.audren_frames_per_buffer == 4);
    assert(resilient.buffer_count == 5);
    assert(resilient.max_writer_wait_frames == 4);
    assert(stream::audioBufferCapacityMs(
               stream::AudioLatencyMode::Resilient) == 100);
    assert(stream::audioIngressQueuePacketLimit(
               stream::AudioLatencyMode::Realtime) == 6);
    assert(stream::audioIngressQueuePacketLimit(
               stream::AudioLatencyMode::Balanced) == 7);
    assert(stream::audioIngressQueuePacketLimit(
               stream::AudioLatencyMode::Resilient) == 7);
    assert(stream::audioStartupPrebufferPackets(
               stream::AudioLatencyMode::Realtime) == 3);
    assert(stream::audioStartupPrebufferPackets(
               stream::AudioLatencyMode::Balanced) == 3);
    assert(stream::audioStartupPrebufferPackets(
               stream::AudioLatencyMode::Resilient) == 4);

    webrtc::NetworkPathEstimate path;
    assert(app::xboxVideoPresentationMode(false, path) ==
           stream::VideoPresentationMode::RealtimeAdaptive);
    path.valid = true;
    path.quality = webrtc::NetworkPathQuality::Good;
    path.observed_quality = webrtc::NetworkPathQuality::Good;
    path.sequence = 1;
    assert(app::xboxVideoPresentationMode(false, path) ==
           stream::VideoPresentationMode::RealtimeAdaptive);
    path.quality = webrtc::NetworkPathQuality::Fair;
    assert(app::xboxVideoPresentationMode(false, path) ==
           stream::VideoPresentationMode::BufferedFifo);
    path.quality = webrtc::NetworkPathQuality::Poor;
    assert(app::xboxVideoPresentationMode(false, path) ==
           stream::VideoPresentationMode::BufferedFifo);
    path.quality = webrtc::NetworkPathQuality::Good;
    app::XboxLatencyController cloud_latency(true);
    auto latency = cloud_latency.observe(path, false);
    assert(latency.mode == app::XboxLatencyMode::Balanced);
    for (int window = 0; window < 8; ++window) {
        ++path.sequence;
        latency = cloud_latency.observe(path, false);
    }
    assert(latency.mode == app::XboxLatencyMode::Realtime);
    assert(latency.video_presentation ==
           stream::VideoPresentationMode::RealtimeAdaptive);
    assert(latency.video_decode_catch_up ==
           stream::VideoDecodeCatchUpMode::Realtime);
    assert(latency.audio_latency == stream::AudioLatencyMode::Realtime);

    path.quality = webrtc::NetworkPathQuality::Poor;
    path.observed_quality = webrtc::NetworkPathQuality::Poor;
    ++path.sequence;
    latency = cloud_latency.observe(path, true);
    assert(latency.mode == app::XboxLatencyMode::Recovery);
    assert(latency.video_presentation ==
           stream::VideoPresentationMode::BufferedFifo);
    assert(latency.audio_latency == stream::AudioLatencyMode::Resilient);
    path.quality = webrtc::NetworkPathQuality::Good;
    path.observed_quality = webrtc::NetworkPathQuality::Good;
    for (int window = 0; window < 4; ++window) {
        ++path.sequence;
        latency = cloud_latency.observe(path, false);
        assert(latency.mode == app::XboxLatencyMode::Recovery);
    }
    ++path.sequence;
    latency = cloud_latency.observe(path, false);
    assert(latency.mode == app::XboxLatencyMode::Balanced);

    // Media recovery can start between one-second path estimates. It must not
    // wait for a new sequence before expanding the protective buffers.
    latency = cloud_latency.observe(path, true);
    assert(latency.mode == app::XboxLatencyMode::Recovery);
    latency = cloud_latency.observe(path, false);
    assert(latency.mode == app::XboxLatencyMode::Recovery);

    app::XboxLatencyController home_latency(false);
    latency = home_latency.state();
    assert(latency.mode == app::XboxLatencyMode::Realtime);
    assert(latency.audio_latency == stream::AudioLatencyMode::Realtime);
    path.quality = webrtc::NetworkPathQuality::Poor;
    path.observed_quality = webrtc::NetworkPathQuality::Poor;
    ++path.sequence;
    latency = home_latency.observe(path, false);
    assert(latency.mode == app::XboxLatencyMode::Balanced);
    assert(latency.audio_latency == stream::AudioLatencyMode::Balanced);
    path.quality = webrtc::NetworkPathQuality::Good;
    path.observed_quality = webrtc::NetworkPathQuality::Good;
    ++path.sequence;
    latency = home_latency.observe(path, false);
    assert(latency.mode == app::XboxLatencyMode::Realtime);
    assert(latency.audio_latency == stream::AudioLatencyMode::Realtime);
    assert(app::xboxAudioLatencyMode(false) ==
           stream::AudioLatencyMode::Realtime);
    assert(app::xboxAudioLatencyMode(true) ==
           stream::AudioLatencyMode::Balanced);
    assert(app::xboxVideoDecodeCatchUpMode(false) ==
           stream::VideoDecodeCatchUpMode::Realtime);
    assert(app::xboxVideoDecodeCatchUpMode(true) ==
           stream::VideoDecodeCatchUpMode::Resilient);

    app::VideoRecoveryRequestPolicy pli;
    assert(pli.shouldRequest(true, 1000));
    pli.recordAttempt(1000);
    assert(!pli.shouldRequest(true, 1299));
    assert(pli.shouldRequest(true, 1300));
    pli.recordAttempt(1300);
    assert(!pli.shouldRequest(true, 1799));
    assert(pli.shouldRequest(true, 1800));
    pli.recordAttempt(1800);
    assert(!pli.shouldRequest(true, 2799));
    assert(pli.shouldRequest(true, 2800));
    assert(!pli.shouldRequest(false, 2801));
    assert(pli.shouldRequest(true, 3000));

    std::cout << "Realtime latency policy tests passed\n";
    return 0;
}
