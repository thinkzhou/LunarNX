#include "src/ps/ps_connection_trace.h"

#include <cassert>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

int main() {
    namespace fs = std::filesystem;
    fs::create_directories("sdmc:/switch/LunarNX");
    const fs::path log_path = "sdmc:/switch/LunarNX/lunarnx.log";
    fs::remove(log_path);

    lunar::ps::PsConnectionTrace trace("remote", "ps5");
    trace.record("session-create", "ok", "elapsed_ms=%d http_status=%d", 12, 200);
    trace.record("control-punch", "failed", "error=%d", 7);

    // The timing-sensitive record path must only touch bounded memory.
    assert(!fs::exists(log_path));

    trace.finish("failed", "control-punch");
    assert(fs::exists(log_path));

    std::ifstream input(log_path);
    std::ostringstream buffer;
    buffer << input.rdbuf();
    const std::string output = buffer.str();
    assert(output.find("trace=1") != std::string::npos);
    assert(output.find("stage=session-create outcome=ok") != std::string::npos);
    assert(output.find("stage=control-punch outcome=failed") != std::string::npos);
    assert(output.find("stage=launch outcome=failed detail=control-punch") !=
           std::string::npos);

    // A terminal trace is immutable and must not append duplicate records.
    const auto size_before = fs::file_size(log_path);
    trace.record("late", "ignored", "value=%d", 1);
    trace.finish("failed", "duplicate");
    assert(fs::file_size(log_path) == size_before);
    return 0;
}
