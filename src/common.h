#pragma once

#ifdef __SWITCH__
#include <switch.h>
#else
// Mock types for desktop testing
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include <map>
#include <memory>
#include <functional>
#include <chrono>
#include <thread>
#include <mutex>
#include <condition_variable>
#endif

namespace lunar {

// Platform abstraction helpers
#ifdef __SWITCH__
inline const char* get_config_path() { return "sdmc:/switch/LunarNX/config.json"; }
inline const char* get_token_path()  { return "sdmc:/switch/LunarNX/token.json"; }
inline const char* get_log_path()    { return "sdmc:/switch/LunarNX/debug.log"; }
inline const char* get_cloud_library_cache_path() { return "sdmc:/switch/LunarNX/xcloud_library_cache.json"; }
#else
inline const char* get_config_path() { return "./config.json"; }
inline const char* get_token_path()  { return "./token.json"; }
inline const char* get_log_path()    { return "./debug.log"; }
inline const char* get_cloud_library_cache_path() { return "./xcloud_library_cache.json"; }
#endif

} // namespace lunar
