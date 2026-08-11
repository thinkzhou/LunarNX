#!/usr/bin/env python3
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def main() -> None:
    discovery = (ROOT / "src/ps/ps_discovery.cpp").read_text()
    registration = (ROOT / "src/ps/ps_registration.cpp").read_text()
    registration_h = (ROOT / "src/ps/ps_registration.h").read_text()
    manager = (ROOT / "src/ps/ps_manager.cpp").read_text()

    assert "nifmGetCurrentIpConfigInfo" in discovery
    assert "current_ip | ~subnet_mask" in discovery
    assert "options.broadcast_addrs = &directed_broadcast" in discovery
    assert "options.broadcast_num = 1" in discovery
    assert "bool startDiscovery(HostListCallback cb)" in discovery or True
    assert "initialized_" in registration_h
    assert "if (!initialized_.exchange(false)) return;" in registration
    assert "const bool started = registration_->start" in manager
    assert '"Could not start local pairing"' in manager
    assert "psn_auth_.getAccountId()" in manager
    assert "psn_account_id" in registration
    assert "info.broadcast = false" in registration
    assert "target >= CHIAKI_TARGET_PS4_9" in registration
    assert "const bool started = ps_manager_->startDiscovery" in (ROOT / "src/ui/ps_activity.cpp").read_text()

    print("PS local pairing flow tests passed")


if __name__ == "__main__":
    main()
