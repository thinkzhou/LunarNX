#pragma once

#include <cerrno>
#include <cstdio>
#include <string>
#include <sys/stat.h>

namespace lunar::steamlink {

// Single-writer settings replacement. Switch fsdev rename does not replace an
// existing destination. Keep the previous file recoverable during the gap.
inline bool commitSettingsFile(const std::string& temporary, const std::string& path,
    int (*renameFile)(const char*, const char*) = std::rename) {
    const auto backup = path + ".bak";
    struct stat info{};
    const bool exists = ::stat(path.c_str(), &info) == 0;
    if (!exists && errno != ENOENT) return false;
    if (exists) {
        if (std::remove(backup.c_str()) != 0 && errno != ENOENT) return false;
        if (renameFile(path.c_str(), backup.c_str()) != 0) return false;
    }
    if (renameFile(temporary.c_str(), path.c_str()) != 0) {
        const int error = errno;
        // If restoration also fails, the loader can still read the backup.
        if (exists) renameFile(backup.c_str(), path.c_str());
        errno = error;
        return false;
    }
    // A stale backup is harmless; do not report a committed save as failed.
    std::remove(backup.c_str());
    return true;
}

inline FILE* openSettingsFile(const std::string& path) {
    FILE* file = std::fopen(path.c_str(), "rb");
    if (!file && errno == ENOENT) file = std::fopen((path + ".bak").c_str(), "rb");
    return file;
}
} // namespace lunar::steamlink
