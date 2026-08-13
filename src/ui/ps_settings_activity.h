#pragma once
#ifdef __SWITCH__

#include <borealis.hpp>
#include "../stream/video_codec.h"
#include <functional>

namespace lunar::ui {

struct PsSettingsSnapshot {
    int width = 1280;
    int height = 720;
    int bitrate_kbps = 10000;
    stream::VideoCodec video_codec = stream::VideoCodec::H264;
};

PsSettingsSnapshot loadPsSettings();
bool savePsSettings(const PsSettingsSnapshot& settings);

class PsSettingsActivity : public brls::Activity {
public:
    using CompletionCallback = std::function<void(const PsSettingsSnapshot&)>;
    PsSettingsActivity(PsSettingsSnapshot settings, CompletionCallback completion = {});
    brls::View* createContentView() override;

private:
    PsSettingsSnapshot settings_;
    CompletionCallback completion_;
    bool closed_ = false;
    void closeSettings();
};

} // namespace lunar::ui
#endif
