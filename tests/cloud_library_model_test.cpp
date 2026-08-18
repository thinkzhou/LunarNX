#include "../src/ui/cloud_library_model.h"

#include <cassert>
#include <iostream>

using lunar::api::CloudTitle;
using lunar::ui::CloudLibraryFilter;
using lunar::ui::CloudLibrarySort;
using lunar::ui::buildCloudLibraryViewModel;

namespace {

CloudTitle title(const char* id, const char* name, const char* publisher) {
    CloudTitle value;
    value.title_id = id;
    value.product_id = id;
    value.name = name;
    value.publisher = publisher;
    return value;
}

} // namespace

int main() {
    const std::vector<CloudTitle> library {
        title("3", "Halo Infinite", "Xbox Game Studios"),
        title("1", "Forza Horizon 5", "Xbox Game Studios"),
        title("2", "Minecraft Dungeons", "Mojang"),
    };
    std::vector<CloudTitle> recent {library[1]};
    std::vector<CloudTitle> newly_added {library[2]};

    auto all = buildCloudLibraryViewModel(
        library, recent, newly_added, "", CloudLibraryFilter::All,
        CloudLibrarySort::Title, 2);
    assert(all.total_matches == 3);
    assert(all.items.size() == 2);
    assert(all.items[0].name == "Forza Horizon 5");
    assert(all.items[0].is_recent);

    auto search = buildCloudLibraryViewModel(
        library, recent, newly_added, "xbox", CloudLibraryFilter::All,
        CloudLibrarySort::Title, 20);
    assert(search.total_matches == 2);

    auto recent_only = buildCloudLibraryViewModel(
        library, recent, newly_added, "", CloudLibraryFilter::Recent,
        CloudLibrarySort::RecentFirst, 20);
    assert(recent_only.items.size() == 1);
    assert(recent_only.items[0].title_id == "1");

    auto new_only = buildCloudLibraryViewModel(
        library, recent, newly_added, "", CloudLibraryFilter::New,
        CloudLibrarySort::Publisher, 20);
    assert(new_only.items.size() == 1);
    assert(new_only.items[0].title_id == "2");

    auto recent_order = buildCloudLibraryViewModel(
        library, {library[2], library[0]}, {}, "", CloudLibraryFilter::All,
        CloudLibrarySort::RecentFirst, 20);
    assert(recent_order.items.size() == library.size());
    assert(recent_order.items[0].title_id == "2");
    assert(recent_order.items[1].title_id == "3");

    std::cout << "cloud library model tests passed\n";
    return 0;
}
