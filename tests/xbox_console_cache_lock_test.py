#!/usr/bin/env python3
from pathlib import Path


def require(condition, message):
    if not condition:
        raise SystemExit(f"FAIL: {message}")


def main():
    source = Path("src/app/stream_controller.cpp").read_text()
    fetch = source.split("bool StreamController::fetchConsoles()", 1)[1].split(
        "std::vector<api::XboxConsole> StreamController::getConsoles()", 1)[0]
    save = source.split("void StreamController::saveConsoleCache() const", 1)[1].split(
        "std::string StreamController::getLastStreamError()", 1)[0]

    require("if (found) saveConsoleCache();" in fetch,
            "successful Xbox discovery must use the lock-safe cache writer")
    require("get_xbox_console_cache_path" not in fetch and "std::fopen" not in fetch,
            "Xbox discovery must not perform cache file I/O while updating lifecycle state")
    require("copy = consoles_;" in save,
            "Xbox cache writer must snapshot consoles under the lifecycle lock")
    lock_end = save.index("if (copy.empty()) return;")
    require("std::fopen" not in save[:lock_end] and "cJSON_CreateObject" not in save[:lock_end],
            "Xbox cache serialization and file I/O must happen after releasing the lock")

    print("Xbox console cache lock test passed")


if __name__ == "__main__":
    main()
