#!/usr/bin/env python3
from pathlib import Path


def require(condition, message):
    if not condition:
        raise SystemExit(f"FAIL: {message}")


def main():
    common = Path("src/common.h").read_text()
    repository_h = Path("src/ps/ps_console_repository.h").read_text()
    repository = Path("src/ps/ps_console_repository.cpp").read_text()
    manager_h = Path("src/ps/ps_manager.h").read_text()
    manager = Path("src/ps/ps_manager.cpp").read_text()
    ui = Path("src/ui/ps_activity.cpp").read_text()

    require("get_ps_console_cache_path" in common,
            "PS remote console cache must have its own storage path")
    require("loadPsnCache" in repository_h and "savePsnCache" in repository_h,
            "PS repository must own persistent PSN console cache serialization")
    require('cJSON_GetObjectItemCaseSensitive(root, "account_id")' in repository and
            "account_id != account->valuestring" in repository,
            "PS cache must be scoped to the signed-in PSN account")
    require('cJSON_AddStringToObject(root, "account_id"' in repository and
            'cJSON_AddItemToObject(root, "consoles"' in repository,
            "PS cache must persist the account and remote console list")
    require("std::rename(temp_path.c_str(), path.c_str())" in repository,
            "PS cache replacement must be atomic")
    require("kMaxCacheBytes" in repository and "length > kMaxCacheBytes" in repository,
            "PS cache loading must reject unexpectedly large files")
    require("bool loadPsnDeviceCache()" in manager_h and
            "repository_->loadPsnCache" in manager,
            "PS manager must expose cache restoration after loading the PSN session")
    require("repository_->savePsnCache" in manager,
            "a successful live PSN lookup must replace the cache")
    require("loadPsnDeviceCache()" in ui and "psn_cache_loaded" in ui,
            "PS UI must restore and display cached remote consoles without a lookup click")
    require("clearPsnDeviceCache()" in ui,
            "sign-out and expired sessions must clear account-scoped PS cache data")

    print("PS console cache tests passed")


if __name__ == "__main__":
    main()
