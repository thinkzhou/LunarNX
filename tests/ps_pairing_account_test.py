#!/usr/bin/env python3
from pathlib import Path


def main():
    header = Path("src/ps/ps_pairing_account.h").read_text()
    source = Path("src/ps/ps_pairing_account.cpp").read_text()
    manager = Path("src/ps/ps_manager.cpp").read_text()

    assert "isValidPsnAccountId" in header
    assert "loadManualPsnAccountId" in header
    assert "saveManualPsnAccountId" in header
    assert "normalizePsnAccountId" in header
    assert "lookupPsnAccountId" in header
    assert '"ps_local_account_id"' in source
    assert "std::stoull" in source
    assert "uid >> (i * 8)" in source
    assert "base64Encode(bytes, sizeof(bytes))" in source
    assert '"https://psn.flipscreen.games/search.php?username="' in source
    assert "getSensitive" in source
    assert '"encoded_id"' in source
    assert "getPairingAccountId" in manager
    assert "loadManualPsnAccountId()" in manager
    pairing_method = manager.split("std::string PsManager::getPairingAccountId() const", 1)[1]
    pairing_method = pairing_method.split("}", 1)[0]
    assert "psn_auth_.getAccountId()" not in pairing_method
    print("PS pairing account tests passed")


if __name__ == "__main__":
    main()
