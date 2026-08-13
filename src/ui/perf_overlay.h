#pragma once
#ifdef __SWITCH__
#include <borealis.hpp>
#include "../app/stream_runtime.h"
#include "../stream/perf_stats.h"
#include <string>

namespace lunar::ui {

/// Semi-transparent performance overlay drawn on top of the stream.
/// Shows FPS, decode latency, network stats, battery.
class PerfOverlay : public brls::Box {
public:
    PerfOverlay(const stream::PerfStats* perf, app::StreamPlatform platform);
    void update(float fps, const std::string& resolution,
                const std::string& video_backend,
                const std::string& video_codec);
    void toggle();
    void setVisible(bool visible);
    bool isVisible() const { return visible_; }

private:
    brls::Label* label_;
    const stream::PerfStats* perf_;
    app::StreamPlatform platform_ = app::StreamPlatform::Xbox;
    bool visible_ = false;
};

} // namespace lunar::ui
#endif
