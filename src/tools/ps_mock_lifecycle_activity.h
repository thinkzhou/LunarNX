#pragma once

#ifdef __SWITCH__

#include <borealis.hpp>

#include <atomic>
#include <memory>
#include <thread>

namespace lunar::tools {

class PsMockLifecycleActivity final : public brls::Activity {
public:
    ~PsMockLifecycleActivity() override;
    brls::View* createContentView() override;

private:
    class ReplayView;
    void run();
    void present();

    std::shared_ptr<class ps_controller_holder> holder_;
    std::thread worker_;
    std::atomic<bool> running_{true};
};

} // namespace lunar::tools

#endif
