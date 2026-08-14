#pragma once

#include <string>

namespace lunar::app {

inline std::string& runningNroPathStorage() {
    static std::string path = "sdmc:/switch/LunarNX/LunarNX.nro";
    return path;
}

inline void setRunningNroPath(const char* path) {
    if (!path || !*path) return;
    const std::string candidate(path);
    if (candidate.rfind("sdmc:/", 0) == 0 && candidate.size() > 4 &&
        candidate.substr(candidate.size() - 4) == ".nro") {
        runningNroPathStorage() = candidate;
    }
}

inline const std::string& runningNroPath() {
    return runningNroPathStorage();
}

} // namespace lunar::app
