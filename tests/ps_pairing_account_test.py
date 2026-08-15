#!/usr/bin/env python3
from pathlib import Path


def main():
    header = Path("src/ps/ps_pairing_account.h").read_text()
    source = Path("src/ps/ps_pairing_account.cpp").read_text()
    manager = Path("src/ps/ps_manager.cpp").read_text()

    assert "isValidPsnAccountId" in header
    assert "loadManualPsnAccountId" in header
    assert "saveManualPsnAccountId" in header
    assert '"ps_local_account_id"' in source
    assert "getPairingAccountId" in manager
    assert "psn_auth_.getAccountId()" in manager
    assert "loadManualPsnAccountId()" in manager
    print("PS pairing account tests passed")


if __name__ == "__main__":
    main()
