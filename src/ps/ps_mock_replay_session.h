#pragma once

#ifdef __SWITCH__

#include "ps_stream_session.h"

#include <chiaki/log.h>
#include <atomic>
#include <string>
#include <thread>

namespace lunar::ps {

bool psMockReplayEnabled();

class PsMockReplaySession {
public:
    PsMockReplaySession(PsMediaBridge& bridge, int fps,
                        stream::VideoCodec video_codec);
    ~PsMockReplaySession();

    PsMockReplaySession(const PsMockReplaySession&) = delete;
    PsMockReplaySession& operator=(const PsMockReplaySession&) = delete;

    bool start(PsSessionCallbacks callbacks);
    void stop();
    std::string lastError() const { return last_error_; }
    void setControllerState(ChiakiControllerState&) {}
    void requestIDR() {}

private:
    void replayLoop();

    PsMediaBridge& bridge_;
    int fps_;
    stream::VideoCodec video_codec_;
    PsSessionCallbacks callbacks_;
    ChiakiLog log_{};
    std::atomic<bool> running_{false};
    std::thread worker_;
    std::string last_error_;
};

} // namespace lunar::ps

#endif
