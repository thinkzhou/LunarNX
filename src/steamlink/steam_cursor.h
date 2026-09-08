#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace lunar::steamlink {

struct CursorImage {
    int width, height, hot_x, hot_y;
    std::vector<uint8_t> rgba;
};
struct CursorSnapshot {
    bool visible = false;
    float x = 0.5f, y = 0.5f;
    int capture_width = 1280, capture_height = 720;
    int video_width = 1280, video_height = 720;
    std::shared_ptr<const CursorImage> image;
};

// Wire images are raw RGBA. Copy only after exact size/hotspot validation.
// Protocol callbacks own no GPU resources; snapshots share immutable pixels.
class SteamCursor {
public:
    void reset(int width, int height) {
        std::lock_guard<std::mutex> lock(mutex_);
        state_ = {}; images_.clear(); selected_ = 0;
        state_.capture_width = width; state_.capture_height = height;
        state_.video_width = width; state_.video_height = height;
    }
    bool select(uint64_t id) {
        std::lock_guard<std::mutex> lock(mutex_);
        selected_ = id;
        auto it = images_.find(id);
        state_.image = it == images_.end() ? nullptr : it->second;
        return bool(state_.image);
    }
    void erase(uint64_t id) {
        std::lock_guard<std::mutex> lock(mutex_);
        images_.erase(id);
        if (id == selected_) state_.image.reset();
    }
    bool image(uint64_t id, int w, int h, int hx, int hy, const uint8_t* bytes, size_t size) {
        if (w <= 0 || h <= 0 || w > 256 || h > 256 || hx < 0 || hy < 0 ||
            hx >= w || hy >= h || !bytes || size != size_t(w) * size_t(h) * 4) return false;
        auto image = std::make_shared<CursorImage>(CursorImage{w,h,hx,hy,{bytes,bytes+size}});
        std::lock_guard<std::mutex> lock(mutex_);
        // Eight images maximum, plus at most one immutable UI snapshot.
        if (!images_.count(id) && images_.size() >= 8) {
            auto victim = images_.begin();
            if (victim->first == selected_) ++victim;
            images_.erase(victim);
        }
        images_[id] = image;
        if (selected_ == id) state_.image = std::move(image);
        return true;
    }
    void show(float x, float y) {
        if (!std::isfinite(x) || !std::isfinite(y)) return;
        std::lock_guard<std::mutex> lock(mutex_);
        state_.visible = true;
        positionLocked(x, y);
    }
    void hide() { std::lock_guard<std::mutex> lock(mutex_); state_.visible = false; }
    void position(float x, float y) {
        if (!std::isfinite(x) || !std::isfinite(y)) return;
        std::lock_guard<std::mutex> lock(mutex_); positionLocked(x, y);
    }
    void move(int dx, int dy) {
        std::lock_guard<std::mutex> lock(mutex_);
        // Local prediction only: host pointer acceleration can differ. The
        // next host ShowCursor message reanchors the normalized position.
        positionLocked(state_.x + float(dx)/state_.capture_width,
                       state_.y + float(dy)/state_.capture_height);
    }
    void captureSize(int width, int height) {
        if (width <= 0 || height <= 0 || width > 16384 || height > 16384) return;
        std::lock_guard<std::mutex> lock(mutex_);
        state_.capture_width = width; state_.capture_height = height;
    }
    CursorSnapshot snapshot() const { std::lock_guard<std::mutex> lock(mutex_); return state_; }
    void videoSize(int width, int height) {
        if (width <= 0 || height <= 0 || width > 16384 || height > 16384) return;
        std::lock_guard<std::mutex> lock(mutex_);
        state_.video_width = width; state_.video_height = height;
    }
private:
    void positionLocked(float x, float y) {
        state_.x = std::clamp(x, 0.0f, 1.0f); state_.y = std::clamp(y, 0.0f, 1.0f);
    }
    mutable std::mutex mutex_;
    CursorSnapshot state_;
    uint64_t selected_ = 0;
    std::unordered_map<uint64_t, std::shared_ptr<const CursorImage>> images_;
};
} // namespace lunar::steamlink
