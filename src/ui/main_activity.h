#pragma once
#ifdef __SWITCH__
#include <borealis.hpp>
#include "../app/stream_controller.h"
#include "recycling_list.hpp"
#include "poster_loader.h"
#include <memory>
#include <atomic>
#include <chrono>
#include <string>
#include <vector>

namespace lunar::ui {

class MainActivity : public brls::Activity {
public:
    MainActivity(std::shared_ptr<app::StreamController> ctrl);
    ~MainActivity();
    brls::View* createContentView() override;
    void onResume() override;

private:
    std::shared_ptr<app::StreamController> ctrl_;
    std::shared_ptr<std::atomic<bool>> alive_ = std::make_shared<std::atomic<bool>>(true);
    std::shared_ptr<std::atomic<bool>> connecting_ = std::make_shared<std::atomic<bool>>(false);
    std::shared_ptr<std::atomic<bool>> fetching_consoles_ = std::make_shared<std::atomic<bool>>(false);
    std::shared_ptr<std::atomic<bool>> fetching_cloud_ = std::make_shared<std::atomic<bool>>(false);
    brls::Label* gamer_tag_ = nullptr;
    brls::ScrollingFrame* scroll_frame_ = nullptr;
    brls::Box* console_list_ = nullptr; // home consoles + non-cloud messages
    RecyclingList* cloud_list_ = nullptr; // xCloud virtualized list
    brls::Label* status_ = nullptr;
    brls::Button* source_xbox_ = nullptr;
    brls::Button* source_cloud_ = nullptr;
    brls::Button* refresh_btn_ = nullptr;
    brls::Button* cloud_search_btn_ = nullptr;
    brls::Label* content_title_ = nullptr;
    brls::Label* content_subtitle_ = nullptr;
    enum class StreamSource { Xbox, Cloud };
    StreamSource stream_source_ = StreamSource::Xbox;
    std::string cloud_search_query_;

    int stream_width_ = 1280;
    int stream_height_ = 720;
    int stream_bitrate_kbps_ = 10000;
    std::string preferred_game_language_ = "en-US";
    stream::VideoBackend video_backend_ = stream::VideoBackend::HardwareZeroCopy;
    stream::PostProcessMode post_process_mode_ = stream::PostProcessMode::Off;
    bool dithering_enabled_ = false;
    bool vibration_enabled_ = true;
    int rumble_strength_percent_ = 50;
    std::chrono::steady_clock::time_point signout_press_time_{};
    std::atomic<bool> signout_pending_{false};
    std::atomic<bool> signout_running_{false};
    std::atomic<bool> mock_auto_connect_attempted_{false};

    void startConsoleStream(const std::string& server_id, const std::string& console_name);
    void startCloudTitleStream(const std::string& title_id, const std::string& title_name);
    void installStateCallback();
    void refreshConsoles();
    void refreshCloudTitles();
    void rebuildCloudList();
    void setCloudListVisible(bool visible);
    void setConsoleListMessage(const std::string& message);
    void setListLoading(const std::string& title, const std::string& detail);
    void setFetchControlsEnabled(bool enabled);
    void setStreamSource(StreamSource source);
    void highlightStreamSource();
    void updateRefreshButtonLabel();
    void promptCloudSearch();
    void updateCloudSearchButtonLabel();
    void confirmSignOut();
    void resetToAuthActivity();
    void refreshCurrentSource();
    void openStreamSettings();
};

} // namespace lunar::ui
#endif
