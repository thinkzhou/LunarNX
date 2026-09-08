#include "steamlink/stream_support.h"
#include "steamlink/steam_cursor.h"
#include <cassert>
#include <fstream>
#include <iostream>
#include <iterator>

using namespace lunar::steamlink;
using namespace std::chrono_literals;

int main(int argc, char** argv) {
    {
        using Timeout = VideoProgressWatchdog::Timeout;
        VideoProgressWatchdog watch;
        assert(watch.observe(1,1,1,1,false,false)==Timeout::None);
        // Continuous audio is intentionally absent from this interface.
        assert(watch.observe(20000000001ULL,1,1,1,false,false)==Timeout::Receive);
        watch.reset(); watch.observe(1,1,1,1,false,false);
        assert(watch.observe(20000000001ULL,2,1,1,false,false)==Timeout::Decode);
        watch.reset(); watch.observe(1,1,1,1,false,false);
        assert(watch.observe(20000000001ULL,2,2,1,false,false)==Timeout::Present);
        watch.reset(); watch.observe(1,1,1,1,false,false);
        watch.observe(2,2,2,2,true,false);
        assert(watch.observe(20000000002ULL,3,3,3,true,false)==Timeout::Recovery);
        assert(watch.observe(20000000003ULL,4,4,4,false,false)==Timeout::None);
        assert(watch.observe(60000000000ULL,4,4,4,false,true)==Timeout::None);
        assert(watch.observe(60000000001ULL,4,4,4,false,false)==Timeout::None);
        assert(watch.observe(80000000000ULL,4,4,4,false,false)==Timeout::Receive);
        watch.reset(); watch.observe(1,0,0,0,false,false);
        for (uint64_t n=1;n<300;++n)
            assert(watch.observe(n*1000000000ULL,n,n,n,false,false)==Timeout::None);
    }
    {
        InputAvailabilityWatchdog watch;
        assert(!watch.expired(1,false));
        assert(watch.expired(20000000001ULL,false));
        assert(!watch.expired(20000000002ULL,true));
        assert(!watch.expired(40000000000ULL,false)); // close gets a grace period
        assert(watch.expired(60000000000ULL,false));
        watch.reset();
        assert(!watch.expired(90000000000ULL,false));
    }
    {
        VideoRecoveryFeedback recovery;
        assert(!recovery.reportLost(true, false, 100));
        assert(recovery.reportLost(true, true, 100)); // accepted but awaiting IDR
        assert(!recovery.reportLost(true, true, 200)); // bounded request rate
        assert(recovery.reportLost(true, true, 1000000100ULL)); // retry until recovered
        assert(!recovery.reportLost(true, false, 3000000000ULL));
        assert(recovery.reportLost(false, false, 3000000000ULL));
        recovery.reset();
        assert(recovery.reportLost(true, true, 1));
        MediaActivityWatchdog watch;
        watch.reset(100);
        assert(!watch.expired(20000000099ULL));
        assert(watch.expired(20000000100ULL));
        watch.received(20000000100ULL);
        watch.received(100); // delayed audio callback must not move time backwards
        assert(!watch.expired(20000000101ULL));
    }
    {
        SteamCursor cursor;
        cursor.reset(1280,720);
        assert(!cursor.select(7));
        const uint8_t pixels[] = {255,0,0,255, 0,255,0,255};
        assert(!cursor.image(7,2,1,2,0,pixels,sizeof(pixels)));
        assert(!cursor.image(7,2,1,0,0,pixels,7));
        assert(!cursor.image(7,2147483647,2147483647,0,0,pixels,8));
        assert(!cursor.image(7,2,1,0,0,nullptr,8));
        assert(cursor.image(7,2,1,1,0,pixels,8));
        assert(cursor.select(7));
        cursor.show(0.5f,0.5f);
        auto snapshot = cursor.snapshot();
        assert(snapshot.visible && snapshot.image->hot_x == 1);
        assert(snapshot.image->rgba[0] == 255 && snapshot.image->rgba[5] == 255);
        cursor.captureSize(1920,1080);
        cursor.videoSize(960,540);
        cursor.move(192,-108);
        assert(std::abs(cursor.snapshot().x-0.6f)<0.0001f);
        assert(std::abs(cursor.snapshot().y-0.4f)<0.0001f);
        cursor.position(-1,2);
        assert(cursor.snapshot().x == 0 && cursor.snapshot().y == 1);
        cursor.show(NAN,0);
        assert(cursor.snapshot().x == 0);
        cursor.hide(); assert(!cursor.snapshot().visible);
        cursor.erase(7); assert(!cursor.snapshot().image);
        assert(snapshot.image->rgba[0] == 255); // UI snapshot survives deletion
        assert(!cursor.select(99));
        assert(cursor.image(7,2,1,0,0,pixels,8));
        assert(!cursor.snapshot().image); // out-of-order image cannot change selection
        for (int i=100;i<200;++i) assert(cursor.image(i,2,1,0,0,pixels,8));
        int retained = cursor.select(7) ? 1 : 0;
        for (int i=100;i<200;++i) retained += cursor.select(i) ? 1 : 0;
        assert(retained <= 8);
        cursor.reset(1280,720);
        assert(!cursor.snapshot().visible && !cursor.snapshot().image);
    }
    {
        StartupWatchdog watch;
        watch.reset(100);
        assert(watch.expired(15000000099ULL) == StartupWatchdog::Timeout::None);
        assert(watch.expired(15000000100ULL) == StartupWatchdog::Timeout::Connection);
        watch.connected(1000);
        watch.connected(2000); // duplicate callbacks cannot extend a deadline
        assert(watch.expired(20000000999ULL) == StartupWatchdog::Timeout::None);
        assert(watch.expired(20000001000ULL) == StartupWatchdog::Timeout::FirstFrame);
        watch.reset(30000000000ULL);
        assert(watch.expired(30000000001ULL) == StartupWatchdog::Timeout::None);
        assert(watch.expired(45000000000ULL) == StartupWatchdog::Timeout::Connection);
    }
    {
        auto* session = new int(42);
        std::vector<int> events;
        auto disconnect = [&](int* p) { assert(*p == 42); events.push_back(1); };
        auto join = [&](int* p) { assert(*p == 42); events.push_back(2); };
        auto destroy = [&](int* p) { events.push_back(3); delete p; };
        closeSession(session, disconnect, join, destroy);
        closeSession(session, disconnect, join, destroy);
        assert(!session && (events == std::vector<int>{1,2,3}));
        bool previous = false;
        std::vector<bool> sent;
        auto send = [&](bool down) { sent.push_back(down); return true; };
        assert(!sendKeyTransition(true, previous, [](bool) { return false; }));
        assert(!previous);
        assert(sendKeyTransition(true, previous, send));
        assert(!sendKeyTransition(true, previous, send));
        assert(sendKeyTransition(false, previous, send));
        assert((sent == std::vector<bool>{true, false}));
    }
    // The production cancellation waiter must wake without a network callback,
    // even while the protocol owner holds its lifecycle lock.
    for (int i = 0; i < 50; ++i) {
        StreamCancellation cancel;
        std::mutex mutex;
        std::condition_variable cv;
        std::atomic<bool> waiting{false};
        std::thread worker([&] {
            std::unique_lock<std::mutex> lock(mutex);
            waiting = true;
            assert(cancel.wait(cv, lock, 20s, [] { return false; }));
            assert(cancel.requested());
        });
        while (!waiting) std::this_thread::yield();
        const auto start = std::chrono::steady_clock::now();
        cancel.request();
        cancel.request();
        worker.join();
        assert(std::chrono::steady_clock::now() - start < 500ms);
    }
    {
        StreamCancellation cancel;
        std::mutex mutex;
        std::condition_variable cv;
        std::unique_lock<std::mutex> lock(mutex);
        assert(!cancel.wait(cv, lock, 20ms, [] { return false; }));
        assert(cancel.wait(cv, lock, 20ms, [] { return true; }));
    }
    // Exercise the same pump/try-lock shutdown contract used by the controller.
    for (int i = 0; i < 30; ++i) {
        InputPump pump;
        std::mutex lifecycle;
        std::atomic<unsigned> sends{0};
        pump.start([&] {
            std::unique_lock<std::mutex> lock(lifecycle, std::try_to_lock);
            if (lock.owns_lock()) ++sends;
        });
        while (sends < 2) std::this_thread::sleep_for(1ms);
        std::lock_guard<std::mutex> lock(lifecycle);
        pump.stop();
        const auto stopped = sends.load();
        pump.stop();
        std::this_thread::sleep_for(10ms);
        assert(sends == stopped);
    }
    {
        AudioProgressMonitor audio;
        using S = AudioProgressMonitor::Status;
        const uint64_t second = 1000000000ULL;
        assert(audio.observe(0, 0, 0, 0, false, false) == S::Healthy);
        assert(audio.observe(20*second, 0, 0, 0, false, false) == S::Missing);
        assert(audio.observe(21*second, 1, 0, 0, false, false) == S::Decode);
        assert(audio.observe(22*second, 2, 1, 0, false, false) == S::Output);
        assert(audio.observe(23*second, 3, 2, 1, false, false) == S::Healthy);
        assert(audio.observe(24*second, 3, 2, 1, true, false) == S::Unsupported);
        assert(audio.observe(100*second, 3, 2, 1, false, true) == S::Healthy);
        assert(audio.observe(101*second, 3, 2, 1, false, false) == S::Healthy);
        for (uint64_t i=102; i<402; ++i)
            assert(audio.observe(i*second, i, i, i, false, false) == S::Healthy);
    }
    std::vector<uint8_t> params;
    const uint8_t avcc[] = {1, 66, 0, 30, 255, 225, 0, 2, 0x67, 1, 1, 0, 2, 0x68, 2};
    assert(h264Parameters(avcc, sizeof(avcc), params));
    assert((params == std::vector<uint8_t>{0,0,0,1,0x67,1,0,0,0,1,0x68,2}));
    for (size_t n = 1; n < sizeof(avcc); ++n) {
        assert(!h264Parameters(avcc, n, params));
        assert(params.empty());
    }
    assert(h264Parameters(nullptr, 0, params));
    assert(!h264Parameters(nullptr, 1, params));
    const uint8_t annex[] = {0,0,0,1,0x67,1,0,0,0,1,0x68,2};
    assert(h264Parameters(annex, sizeof(annex), params));
    const uint8_t idr[] = {0,0,0,1,0x65,3};
    const auto unit = h264AccessUnit(params, idr, sizeof(idr));
    assert(unit.size() == sizeof(annex) + sizeof(idr));
    LogThrottle throttle;
    assert(throttle.allow(1000));
    for (int i = 0; i < 10000; ++i) assert(!throttle.allow(1001));
    assert(throttle.allow(2000));
    // Optional real H.264 fixture: transport supplies SPS/PPS separately.
    if (argc == 4) {
        auto read = [](const char* path) {
            std::ifstream file(path, std::ios::binary);
            return std::vector<uint8_t>(std::istreambuf_iterator<char>(file), {});
        };
        const auto config = read(argv[1]);
        const auto frames = read(argv[2]);
        assert(!config.empty() && !frames.empty());
        assert(h264Parameters(config.data(), config.size(), params));
        const auto output = h264AccessUnit(params, frames.data(), frames.size());
        std::ofstream file(argv[3], std::ios::binary);
        file.write(reinterpret_cast<const char*>(output.data()), output.size());
    }
    std::cout << "PASS: cursor validation/cache/position/lifetime, startup/audio deadlines, cancellation, input pump, key retry/release, single session destruction, H264 config, log throttle\n";
}
