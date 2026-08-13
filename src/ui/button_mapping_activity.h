#pragma once
#ifdef __SWITCH__

#include <borealis.hpp>
#include <switch.h>

#include "../input/button_mapping.h"

#include <array>
#include <cstddef>
#include <vector>

namespace lunar::ui {

class ButtonMappingActivity : public brls::Activity {
public:
    explicit ButtonMappingActivity(input::ButtonMappingProfile profile);
    ~ButtonMappingActivity() override;

    brls::View* createContentView() override;
    void pollCaptureInput();

private:
    void enterCapture(size_t index);
    void finishCapture();
    void refreshRows();
    bool hasConflict(size_t index) const;

    input::ButtonMapping mapping_;
    input::ButtonMappingProfile profile_;
    std::vector<brls::DetailCell*> rows_;
    brls::Box* mapping_content_ = nullptr;
    brls::Box* capture_content_ = nullptr;
    brls::Label* capture_status_ = nullptr;
    PadState capture_pad_{};
    size_t capture_index_ = 0;
    uint64_t peak_buttons_ = 0;
    bool capturing_ = false;
    bool waiting_for_release_ = false;
    bool saw_button_ = false;
    int release_frames_ = 0;
};

} // namespace lunar::ui
#endif
