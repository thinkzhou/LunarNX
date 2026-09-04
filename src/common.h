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
inline const char* get_cover_cache_dir() { return "sdmc:/switch/LunarNX/cache/covers"; }
inline const char* get_ps_credentials_path() { return "sdmc:/switch/LunarNX/ps_credentials.json"; }
inline const char* get_psn_token_path() { return "sdmc:/switch/LunarNX/psn_token.json"; }
inline const char* get_ps_console_cache_path() { return "sdmc:/switch/LunarNX/ps_console_cache.json"; }
inline const char* get_ps_route_preferences_path() { return "sdmc:/switch/LunarNX/ps_route_preferences.json"; }
inline const char* get_psn_callback_import_path() { return "sdmc:/switch/LunarNX/psn_callback.txt"; }
inline const char* get_xbox_console_cache_path() { return "sdmc:/switch/LunarNX/xbox_console_cache.json"; }
inline const char* get_xbox_ice_preferences_path() { return "sdmc:/switch/LunarNX/xbox_ice_preferences.json"; }
#else
inline const char* get_config_path() { return "./config.json"; }
inline const char* get_token_path()  { return "./token.json"; }
inline const char* get_log_path()    { return "./debug.log"; }
inline const char* get_cloud_library_cache_path() { return "./xcloud_library_cache.json"; }
inline const char* get_cover_cache_dir() { return "./cache/covers"; }
inline const char* get_ps_credentials_path() { return "./ps_credentials.json"; }
inline const char* get_psn_token_path() { return "./psn_token.json"; }
inline const char* get_ps_console_cache_path() { return "./ps_console_cache.json"; }
inline const char* get_ps_route_preferences_path() { return "./ps_route_preferences.json"; }
inline const char* get_psn_callback_import_path() { return "./psn_callback.txt"; }
inline const char* get_xbox_console_cache_path() { return "./xbox_console_cache.json"; }
inline const char* get_xbox_ice_preferences_path() { return "./xbox_ice_preferences.json"; }
#endif

} // namespace lunar
