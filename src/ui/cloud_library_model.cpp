#include "cloud_library_model.h"

#include <algorithm>
#include <cctype>
#include <set>
#include <unordered_map>

namespace lunar::ui {
namespace {

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
        [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return value;
}

std::string identity(const api::CloudTitle& title) {
    if (!title.product_id.empty()) return "p:" + lower(title.product_id);
    if (!title.title_id.empty()) return "t:" + lower(title.title_id);
    return "n:" + lower(title.name);
}

std::set<std::string> identities(const std::vector<api::CloudTitle>& titles) {
    std::set<std::string> result;
    for (const auto& title : titles) result.insert(identity(title));
    return result;
}

} // namespace

CloudLibraryViewModel buildCloudLibraryViewModel(
    const std::vector<api::CloudTitle>& library,
    const std::vector<api::CloudTitle>& recent,
    const std::vector<api::CloudTitle>& newly_added,
    const std::string& query,
    CloudLibraryFilter filter,
    CloudLibrarySort sort,
    size_t visible_limit) {
    const auto recent_ids = identities(recent);
    const auto new_ids = identities(newly_added);
    std::unordered_map<std::string, size_t> recent_rank;
    for (size_t index = 0; index < recent.size(); ++index) {
        recent_rank.emplace(identity(recent[index]), index);
    }
    const std::string normalized_query = lower(query);

    CloudLibraryViewModel result;
    result.items.reserve(std::min(library.size(), visible_limit));
    std::vector<api::CloudTitle> matches;
    matches.reserve(library.size());

    for (const auto& title : library) {
        const std::string id = identity(title);
        const bool is_recent = title.is_recent || recent_ids.count(id) != 0;
        const bool is_new = new_ids.count(id) != 0;
        const bool filter_match =
            filter == CloudLibraryFilter::All ||
            (filter == CloudLibraryFilter::Recent && is_recent) ||
            (filter == CloudLibraryFilter::New && is_new);
        const bool query_match = normalized_query.empty() ||
            lower(title.name + " " + title.publisher).find(normalized_query) !=
                std::string::npos;
        if (!filter_match || !query_match) continue;

        auto item = title;
        item.is_recent = is_recent;
        matches.push_back(std::move(item));
    }

    auto by_title = [](const api::CloudTitle& left, const api::CloudTitle& right) {
        return lower(left.name) < lower(right.name);
    };
    if (sort == CloudLibrarySort::Publisher) {
        std::stable_sort(matches.begin(), matches.end(),
            [&by_title](const api::CloudTitle& left, const api::CloudTitle& right) {
                const std::string left_publisher = lower(left.publisher);
                const std::string right_publisher = lower(right.publisher);
                return left_publisher == right_publisher
                    ? by_title(left, right)
                    : left_publisher < right_publisher;
            });
    } else if (sort == CloudLibrarySort::RecentFirst) {
        std::stable_sort(matches.begin(), matches.end(),
            [&by_title, &recent_rank](const api::CloudTitle& left,
                                      const api::CloudTitle& right) {
                if (left.is_recent != right.is_recent) {
                    return left.is_recent && !right.is_recent;
                }
                if (left.is_recent) {
                    const auto left_rank = recent_rank.find(identity(left));
                    const auto right_rank = recent_rank.find(identity(right));
                    const size_t left_value = left_rank == recent_rank.end()
                        ? recent_rank.size() : left_rank->second;
                    const size_t right_value = right_rank == recent_rank.end()
                        ? recent_rank.size() : right_rank->second;
                    if (left_value != right_value) return left_value < right_value;
                }
                return by_title(left, right);
            });
    } else {
        std::stable_sort(matches.begin(), matches.end(), by_title);
    }

    result.total_matches = matches.size();
    if (matches.size() > visible_limit) matches.resize(visible_limit);
    result.items = std::move(matches);
    return result;
}

const char* cloudLibraryFilterKey(CloudLibraryFilter filter) {
    switch (filter) {
        case CloudLibraryFilter::Recent: return "lunarnx/main/filter_recent";
        case CloudLibraryFilter::New: return "lunarnx/main/filter_new";
        case CloudLibraryFilter::All:
        default: return "lunarnx/main/filter_all";
    }
}

const char* cloudLibrarySortKey(CloudLibrarySort sort) {
    switch (sort) {
        case CloudLibrarySort::Publisher: return "lunarnx/main/sort_publisher";
        case CloudLibrarySort::RecentFirst: return "lunarnx/main/sort_recent";
        case CloudLibrarySort::Title:
        default: return "lunarnx/main/sort_title";
    }
}

CloudLibraryFilter nextCloudLibraryFilter(CloudLibraryFilter filter) {
    switch (filter) {
        case CloudLibraryFilter::All: return CloudLibraryFilter::Recent;
        case CloudLibraryFilter::Recent: return CloudLibraryFilter::New;
        case CloudLibraryFilter::New:
        default: return CloudLibraryFilter::All;
    }
}

CloudLibrarySort nextCloudLibrarySort(CloudLibrarySort sort) {
    switch (sort) {
        case CloudLibrarySort::Title: return CloudLibrarySort::RecentFirst;
        case CloudLibrarySort::RecentFirst: return CloudLibrarySort::Publisher;
        case CloudLibrarySort::Publisher:
        default: return CloudLibrarySort::Title;
    }
}

} // namespace lunar::ui
