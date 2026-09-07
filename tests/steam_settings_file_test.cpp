#include "steamlink/settings_file.h"
#include <cassert>
#include <cstring>
#include <fstream>
#include <iostream>

static bool failCommit = false;
static bool failRestore = false;
static int switchRename(const char* from, const char* to) {
    struct stat info{};
    if (::stat(to, &info) == 0) { errno = EEXIST; return -1; }
    if ((failCommit && std::strstr(from, ".tmp")) ||
        (failRestore && std::strstr(from, ".bak"))) { errno = EIO; return -1; }
    return std::rename(from, to);
}
static void write(const std::string& path, const char* data) {
    std::ofstream file(path); file << data; file.close(); assert(file.good());
}
static std::string read(const std::string& path) {
    FILE* file = lunar::steamlink::openSettingsFile(path);
    assert(file); char buffer[64]{}; std::fread(buffer, 1, 63, file); std::fclose(file);
    return buffer;
}
int main(int argc, char** argv) {
    assert(argc == 2);
    const std::string path = std::string(argv[1]) + "/settings";
    const auto tmp = path + ".tmp";
    write(tmp, "first");
    assert(lunar::steamlink::commitSettingsFile(tmp, path, switchRename));
    write(tmp, "second");
    // Reproduce the old implementation's failure on an existing destination.
    assert(switchRename(tmp.c_str(), path.c_str()) == -1 && errno == EEXIST);
    assert(lunar::steamlink::commitSettingsFile(tmp, path, switchRename));
    assert(read(path) == "second");
    write(tmp, "third"); failCommit = true;
    assert(!lunar::steamlink::commitSettingsFile(tmp, path, switchRename));
    assert(errno == EIO && read(path) == "second");
    failRestore = true;
    assert(!lunar::steamlink::commitSettingsFile(tmp, path, switchRename));
    assert(read(path) == "second"); // backup fallback after failed restoration
    failCommit = failRestore = false;
    assert(lunar::steamlink::commitSettingsFile(tmp, path, switchRename));
    assert(read(path) == "third");
    for (int i = 0; i < 10; ++i) {
        write(tmp, "repeat");
        assert(lunar::steamlink::commitSettingsFile(tmp, path, switchRename));
        assert(read(path) == "repeat");
    }
    assert(!lunar::steamlink::commitSettingsFile(tmp, path, switchRename));
    assert(read(path) == "repeat"); // missing temporary file preserves current settings
    std::cout << "PASS: first/repeated saves, no-overwrite rename, failed commit/restore, backup recovery\n";
}
