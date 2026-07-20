#pragma once

#include <borealis.hpp>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace lunar::ui {

// Minimal wiliwili-style vertical recycling list (spanCount=1).
// Only creates views for the visible window + prefetch buffer.
class RecyclingListItem : public brls::Box {
public:
    RecyclingListItem();
    ~RecyclingListItem() override = default;

    size_t getIndex() const { return index_; }
    void setIndex(size_t index) { index_ = index; }
    const std::string& getReuseId() const { return reuse_id_; }
    void setReuseId(std::string id) { reuse_id_ = std::move(id); }

    virtual void prepareForReuse() {}
    virtual void cacheForReuse() {}

private:
    size_t index_ = 0;
    std::string reuse_id_;
};

class RecyclingListDataSource {
public:
    virtual ~RecyclingListDataSource() = default;
    virtual size_t getItemCount() const = 0;
    virtual float heightForRow(size_t index) const = 0;
    virtual RecyclingListItem* cellForRow(class RecyclingList* list, size_t index) = 0;
    virtual void onItemSelected(size_t index) {}
};

class RecyclingList : public brls::ScrollingFrame {
public:
    RecyclingList();
    ~RecyclingList() override;

    void setDataSource(RecyclingListDataSource* source); // takes ownership
    RecyclingListDataSource* getDataSource() const { return data_source_; }

    void registerCell(const std::string& identifier,
                      std::function<RecyclingListItem*()> allocator);
    RecyclingListItem* dequeueReusableCell(const std::string& identifier);

    void reloadData();
    void clearData();
    size_t getItemCount() const;

    void draw(NVGcontext* vg, float x, float y, float width, float height,
              brls::Style style, brls::FrameContext* ctx) override;
    void onLayout() override;

    // Prefetch rows above/below viewport.
    void setPrefetchRows(size_t rows) { prefetch_rows_ = rows; }

private:
    void itemsRecyclingLoop();
    void queueReusableCell(RecyclingListItem* cell);
    void removeCell(RecyclingListItem* cell);
    void addCellAt(size_t index);
    float offsetForIndex(size_t index) const;
    float totalContentHeight() const;

    RecyclingListDataSource* data_source_ = nullptr;
    brls::Box* content_box_ = nullptr;
    bool layouted_ = false;
    size_t prefetch_rows_ = 3;
    size_t visible_min_ = 0;
    size_t visible_max_ = 0; // inclusive, or npos if empty
    std::vector<float> height_cache_;
    std::vector<float> offset_cache_;
    std::unordered_map<std::string, std::vector<RecyclingListItem*>> reuse_pool_;
    std::unordered_map<std::string, std::function<RecyclingListItem*()>> allocators_;
};

} // namespace lunar::ui
