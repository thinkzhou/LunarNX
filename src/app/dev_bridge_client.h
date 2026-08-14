#pragma once

#include <string>
#include <vector>
#include <functional>

namespace lunar::app {

struct DevBuild {
    std::string version;
    std::string notes;
    std::string build_id;
    std::string git_commit;
    std::string published_at;
    std::string sha256;
    std::string download_url;
    long long size = 0;
};

class DevBridgeClient {
public:
    using ProgressCallback = std::function<void(long long, long long)>;
    static constexpr const char* kBaseUrl =
        "https://lunarnx-dev-bridge.zy741870701.workers.dev";

    bool fetchVersions(std::vector<DevBuild>& builds, std::string& error) const;
    bool install(const DevBuild& build, const std::string& target_path,
                 std::string& error, ProgressCallback progress = {}) const;
    bool uploadLog(const std::string& log_path, std::string& log_id,
                   std::string& error) const;
};

} // namespace lunar::app
