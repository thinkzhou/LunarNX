// Native regression for idle waits and the sticky-failure busy-loop bug.
// Real queue delivery/startup gates are covered by the mock lifecycle probe.
#include "webrtc/libdatachannel_peer.h"
#include <cassert>
#include <chrono>

int main() {
    using namespace std::chrono;
    lunar::webrtc::LibdatachannelPeer peer;
    PeerConfiguration config{};
    config.video_codec = CODEC_H264;
    int failures = 0;
    config.user_data = &failures;
    assert(peer.initialize(config, nullptr,
        [](PeerConnectionState state, void* user) {
            if (state == PEER_CONNECTION_FAILED) ++*static_cast<int*>(user);
        }, nullptr));
    peer.setMediaEnabled(false);
    auto started = steady_clock::now();
    peer.waitForEventsUntil(started + milliseconds(40));
    assert(steady_clock::now() - started >= milliseconds(30));

    // Invalid SDP latches failed. It should wake until loop() consumes the
    // failure, but it must not permanently make every future wait runnable.
    peer.setRemoteAnswer("invalid SDP");
    peer.waitForEventsUntil(steady_clock::now() + seconds(1));
    peer.loop();
    assert(failures == 1);
    started = steady_clock::now();
    peer.waitForEventsUntil(started + milliseconds(40));
    assert(steady_clock::now() - started >= milliseconds(30));
    peer.loop();
    assert(failures == 1);
    peer.close();
    peer.waitForEventsUntil(steady_clock::now() + seconds(1));
    return 0;
}
