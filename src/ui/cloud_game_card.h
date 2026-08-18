#pragma once

#ifdef __SWITCH__
#include "../api/xbox_api_client.h"
#include "poster_loader.h"

#include <borealis.hpp>
#include <functional>

namespace lunar::ui {

class CloudGameCard : public brls::Box {
public:
    using SelectHandler = std::function<void(const api::CloudTitle&)>;

    CloudGameCard(api::CloudTitle title, bool is_new,
                  PosterLoader::BatchId poster_batch,
                  SelectHandler on_select);

    void onFocusGained() override;
    void onFocusLost() override;

private:
    void updateChrome(bool focused);

    api::CloudTitle title_;
};

} // namespace lunar::ui
#endif
