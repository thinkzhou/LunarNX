#include "../src/diagnostics.h"

#include <cassert>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>

int main() {
    std::remove(lunar::get_diagnostic_log_path());
    lunar::persistentEventLog(
        "ps-registration", "failed stage=%s target=%d", "console-rejected", 1000);
    std::ifstream input(lunar::get_diagnostic_log_path());
    const std::string log((std::istreambuf_iterator<char>(input)),
                          std::istreambuf_iterator<char>());
    assert(log.find("[ps-registration]") != std::string::npos);
    assert(log.find("stage=console-rejected") != std::string::npos);
    assert(log.find("target=1000") != std::string::npos);
    std::remove(lunar::get_diagnostic_log_path());
    return 0;
}
