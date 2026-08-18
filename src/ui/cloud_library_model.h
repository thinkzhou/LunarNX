#pragma once

#include "../api/xbox_api_client.h"

#include <cstddef>
#include <string>
#include <vector>

namespace lunar::ui {

enum class CloudLibraryFilter {
    All,
    Recent,
    New,
};

enum class CloudLibrarySort {
    Title,
    Publisher,
    RecentFirst,
};

struct CloudLibraryViewModel {
    std::vector<api::CloudTitle> items;
    size_t total_matches = 0;
};

CloudLibraryViewModel buildCloudLibraryViewModel(
    const std::vector<api::CloudTitle>& library,
    const std::vector<api::CloudTitle>& recent,
    const std::vector<api::CloudTitle>& newly_added,
    const std::string& query,
    CloudLibraryFilter filter,
    CloudLibrarySort sort,
    size_t visible_limit);

const char* cloudLibraryFilterKey(CloudLibraryFilter filter);
const char* cloudLibrarySortKey(CloudLibrarySort sort);
CloudLibraryFilter nextCloudLibraryFilter(CloudLibraryFilter filter);
CloudLibrarySort nextCloudLibrarySort(CloudLibrarySort sort);

} // namespace lunar::ui
