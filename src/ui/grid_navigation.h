#pragma once

#include <algorithm>
#include <cstddef>

#ifdef __SWITCH__
#include <borealis.hpp>
#include <vector>
#endif

namespace lunar::ui {

constexpr size_t gridTargetColumn(size_t source_column, size_t target_row_size) {
    return target_row_size == 0
        ? 0
        : std::min(source_column, target_row_size - 1);
}

#ifdef __SWITCH__
inline void wireVerticalGridNavigation(
    const std::vector<std::vector<brls::View*>>& rows) {
    for (size_t row = 0; row < rows.size(); ++row) {
        for (size_t column = 0; column < rows[row].size(); ++column) {
            brls::View* view = rows[row][column];
            if (!view) continue;

            // Reset routes first so persistent toolbar controls never retain a
            // pointer to a card or paging button destroyed by a list rebuild.
            view->setCustomNavigationRoute(brls::FocusDirection::UP, view);
            view->setCustomNavigationRoute(brls::FocusDirection::DOWN, view);
            if (row > 0 && !rows[row - 1].empty()) {
                view->setCustomNavigationRoute(
                    brls::FocusDirection::UP,
                    rows[row - 1][gridTargetColumn(column, rows[row - 1].size())]);
            }
            if (row + 1 < rows.size() && !rows[row + 1].empty()) {
                view->setCustomNavigationRoute(
                    brls::FocusDirection::DOWN,
                    rows[row + 1][gridTargetColumn(column, rows[row + 1].size())]);
            }
        }
    }
}
#endif

} // namespace lunar::ui
