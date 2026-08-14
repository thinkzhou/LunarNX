#pragma once
#ifdef __SWITCH__

#include <borealis.hpp>

#include "../app/stream_controller.h"

#include <functional>
#include <memory>

namespace lunar::ui {

struct StreamSettingsSnapshot {
    int width = 1280;
    int height = 720;
    int bitrate_kbps = 10000;
    std::string preferred_game_language = "en-US";
    stream::VideoBackend video_backend = stream::VideoBackend::HardwareZeroCopy;
    stream::PostProcessMode post_process_mode = stream::PostProcessMode::Off;
    bool dithering_enabled = false;
    bool vibration_enabled = true;
    int rumble_strength_percent = 50;
};

enum class StreamSettingsScope {
    Global,
    Xbox,
};

StreamSettingsSnapshot loadStreamSettings();
bool saveStreamSettings(const StreamSettingsSnapshot& settings);

class StreamSettingsActivity : public brls::Activity {
public:
    using CompletionCallback = std::function<void(const StreamSettingsSnapshot&)>;

    StreamSettingsActivity(std::shared_ptr<app::StreamController> ctrl,
                           StreamSettingsSnapshot settings,
                           CompletionCallback completion,
                           StreamSettingsScope scope = StreamSettingsScope::Xbox);

    brls::View* createContentView() override;

private:
    std::shared_ptr<app::StreamController> ctrl_;
    StreamSettingsSnapshot settings_;
    CompletionCallback completion_;
    StreamSettingsScope scope_ = StreamSettingsScope::Xbox;
    bool closed_ = false;

    void closeSettings();
};

} // namespace lunar::ui
#endif
