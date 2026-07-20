#include "recycling_list.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace lunar::ui {
namespace {

constexpr size_t kNpos = std::numeric_limits<size_t>::max();

} // namespace

RecyclingListItem::RecyclingListItem() {
    this->setFocusable(true);
    this->setAxis(brls::Axis::ROW);
    this->registerClickAction([this](brls::View*) {
        auto* parent = dynamic_cast<brls::Box*>(this->getParent());
        if (!parent) return false;
        auto* list = dynamic_cast<RecyclingList*>(parent->getParent());
        if (!list || !list->getDataSource()) return false;
        list->getDataSource()->onItemSelected(getIndex());
        return true;
    });
}

RecyclingList::RecyclingList() {
    this->setFocusable(false);
    this->setScrollingBehavior(brls::ScrollingBehavior::CENTERED);
    content_box_ = new brls::Box(brls::Axis::COLUMN);
    this->setContentView(content_box_);
    visible_min_ = 0;
    visible_max_ = kNpos;
}

RecyclingList::~RecyclingList() {
    delete data_source_;
    data_source_ = nullptr;
    for (auto& [id, pool] : reuse_pool_) {
        for (auto* item : pool) {
            if (!item) continue;
            item->setParent(nullptr);
            delete item;
        }
    }
    reuse_pool_.clear();
}

void RecyclingList::setDataSource(RecyclingListDataSource* source) {
    if (data_source_) {
        delete data_source_;
    }
    data_source_ = source;
    if (layouted_) {
        reloadData();
    }
}

void RecyclingList::registerCell(const std::string& identifier,
                                 std::function<RecyclingListItem*()> allocator) {
    allocators_[identifier] = std::move(allocator);
    reuse_pool_.emplace(identifier, std::vector<RecyclingListItem*>{});
}

RecyclingListItem* RecyclingList::dequeueReusableCell(const std::string& identifier) {
    auto pool_it = reuse_pool_.find(identifier);
    RecyclingListItem* cell = nullptr;
    if (pool_it != reuse_pool_.end() && !pool_it->second.empty()) {
        cell = pool_it->second.back();
        pool_it->second.pop_back();
    } else {
        auto alloc_it = allocators_.find(identifier);
        if (alloc_it == allocators_.end()) {
            return nullptr;
        }
        cell = alloc_it->second();
        if (!cell) return nullptr;
        cell->setReuseId(identifier);
        cell->detach();
    }
    cell->prepareForReuse();
    return cell;
}

void RecyclingList::queueReusableCell(RecyclingListItem* cell) {
    if (!cell) return;
    cell->cacheForReuse();
    reuse_pool_[cell->getReuseId()].push_back(cell);
}

void RecyclingList::removeCell(RecyclingListItem* cell) {
    if (!cell || !content_box_) return;
    auto& children = content_box_->getChildren();
    auto it = std::find(children.begin(), children.end(), cell);
    if (it == children.end()) return;
    children.erase(it);
    cell->willDisappear(true);
}

float RecyclingList::offsetForIndex(size_t index) const {
    if (index >= offset_cache_.size()) return 0.f;
    return offset_cache_[index];
}

float RecyclingList::totalContentHeight() const {
    if (height_cache_.empty()) return 0.f;
    return offset_cache_.back() + height_cache_.back();
}

size_t RecyclingList::getItemCount() const {
    return data_source_ ? data_source_->getItemCount() : 0;
}

void RecyclingList::clearData() {
    if (!data_source_) return;
    // Ownership stays with list; source clears its own model.
    // Caller should replace datasource for full clear if needed.
    reloadData();
}

void RecyclingList::reloadData() {
    if (!content_box_) return;

    // Recycle currently mounted cells.
    auto& children = content_box_->getChildren();
    for (auto* child : children) {
        auto* item = dynamic_cast<RecyclingListItem*>(child);
        if (!item) continue;
        queueReusableCell(item);
        item->willDisappear(true);
    }
    children.clear();

    height_cache_.clear();
    offset_cache_.clear();
    visible_min_ = 0;
    visible_max_ = kNpos;

    setContentOffsetY(0, false);
    if (!data_source_ || data_source_->getItemCount() == 0) {
        content_box_->setHeight(0);
        return;
    }

    const size_t n = data_source_->getItemCount();
    height_cache_.resize(n);
    offset_cache_.resize(n);
    float y = 0.f;
    for (size_t i = 0; i < n; ++i) {
        float h = data_source_->heightForRow(i);
        if (!(h > 0.f)) h = 96.f;
        height_cache_[i] = h;
        offset_cache_[i] = y;
        y += h;
    }
    content_box_->setHeight(y);

    // Mount first visible window starting at 0.
    addCellAt(0);
    itemsRecyclingLoop();
    this->invalidate();
}

void RecyclingList::addCellAt(size_t index) {
    if (!data_source_ || !content_box_) return;
    if (index >= data_source_->getItemCount()) return;

    // Already mounted?
    for (auto* child : content_box_->getChildren()) {
        auto* item = dynamic_cast<RecyclingListItem*>(child);
        if (item && item->getIndex() == index) return;
    }

    RecyclingListItem* cell = data_source_->cellForRow(this, index);
    if (!cell) return;
    cell->setIndex(index);
    cell->setWidth(this->getWidth());
    cell->setHeight(height_cache_[index]);
    cell->setDetachedPosition(0.f, offset_cache_[index]);
    cell->setParent(content_box_);
    content_box_->getChildren().push_back(cell);
    cell->willAppear(true);

    if (visible_max_ == kNpos) {
        visible_min_ = index;
        visible_max_ = index;
    } else {
        visible_min_ = std::min(visible_min_, index);
        visible_max_ = std::max(visible_max_, index);
    }
}

void RecyclingList::itemsRecyclingLoop() {
    if (!data_source_ || !content_box_ || height_cache_.empty()) return;

    const size_t n = height_cache_.size();
    const brls::Rect visible = getVisibleFrame();
    if (!(visible.getHeight() > 0.f)) return;

    // Estimate prefetch height.
    float avg_h = totalContentHeight() / static_cast<float>(n);
    if (!(avg_h > 1.f)) avg_h = 96.f;
    const float pad = avg_h * static_cast<float>(prefetch_rows_);
    const float win_top = visible.getMinY() - pad;
    const float win_bottom = visible.getMaxY() + pad;

    // Find first/last indices intersecting window.
    size_t first = 0;
    while (first + 1 < n && offset_cache_[first] + height_cache_[first] < win_top) {
        ++first;
    }
    size_t last = first;
    while (last + 1 < n && offset_cache_[last] < win_bottom) {
        ++last;
    }

    // Remove cells outside [first, last].
    auto& children = content_box_->getChildren();
    for (size_t i = 0; i < children.size();) {
        auto* item = dynamic_cast<RecyclingListItem*>(children[i]);
        if (!item) {
            ++i;
            continue;
        }
        const size_t idx = item->getIndex();
        if (idx < first || idx > last) {
            queueReusableCell(item);
            removeCell(item);
            continue;
        }
        ++i;
    }

    // Ensure all cells in window are mounted.
    for (size_t idx = first; idx <= last && idx < n; ++idx) {
        addCellAt(idx);
    }

    visible_min_ = first;
    visible_max_ = last;
}

void RecyclingList::draw(NVGcontext* vg, float x, float y, float width, float height,
                         brls::Style style, brls::FrameContext* ctx) {
    // Keep window in sync while scrolling/focus moving.
    itemsRecyclingLoop();
    brls::ScrollingFrame::draw(vg, x, y, width, height, style, ctx);
}

void RecyclingList::onLayout() {
    brls::ScrollingFrame::onLayout();
    if (!content_box_) return;
    content_box_->setWidth(this->getWidth());
    layouted_ = true;
    // Width may change after first layout; remount with correct widths.
    if (data_source_ && content_box_->getChildren().empty() && data_source_->getItemCount() > 0) {
        reloadData();
    } else {
        // Update mounted cell widths.
        for (auto* child : content_box_->getChildren()) {
            auto* item = dynamic_cast<RecyclingListItem*>(child);
            if (!item) continue;
            item->setWidth(this->getWidth());
            if (item->getIndex() < offset_cache_.size()) {
                item->setDetachedPosition(0.f, offset_cache_[item->getIndex()]);
            }
        }
    }
}

} // namespace lunar::ui
