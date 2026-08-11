#!/usr/bin/env python3
from pathlib import Path


def require(condition, message):
    if not condition:
        raise SystemExit(f"FAIL: {message}")


def main():
    console = Path("src/ps/ps_console.h").read_text()
    credentials = Path("src/ps/ps_credentials.cpp").read_text()
    credentials_header = Path("src/ps/ps_credentials.h").read_text()
    registration = Path("src/ps/ps_registration.cpp").read_text()
    manager = Path("src/ps/ps_manager.cpp").read_text()
    repository = Path("src/ps/ps_console_repository.cpp").read_text()

    require("server_mac" in console and "psn_duid" in console,
            "console identities must separate server MAC and PSN DUID")
    require("findByMac" in credentials_header and
            "std::optional<RegisteredCredential>" in credentials_header,
            "credential lookup must return a MAC-keyed copy")
    require('cJSON_AddNumberToObject(root, "version", 2)' in credentials,
            "credentials must use schema version 2")
    require('"console_pin"' not in credentials,
            "legacy pairing PIN must not be persisted or migrated")
    require('"console_login_pin"' in credentials,
            "optional console login PIN must have a distinct field")
    require('path + ".tmp"' in credentials and "std::rename" in credentials,
            "credential writes must be atomic")
    require("info.pin = pin" in registration and "info.console_pin = 0" in registration,
            "pairing PIN and console login PIN must not be conflated")
    require("macFromBytes" in manager,
            "registration result MAC must become the credential key")
    require("setRegisteredCredentials" in manager and
            "setRegisteredCredentials" in repository,
            "repository output must be hydrated with stored credentials")
    require("find(psn.stable_id.substr" not in repository,
            "binary DUID prefix matching must be removed")
    require("it->target = psn.target" in repository and
            "if (it->psn_duid.empty()) it->psn_duid = psn.psn_duid" in repository,
            "PSN merge must update the target and preserve the PSN DUID")

    print("PS identity persistence tests passed")


if __name__ == "__main__":
    main()
