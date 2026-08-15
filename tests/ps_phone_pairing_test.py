#!/usr/bin/env python3
from pathlib import Path


def main():
    header = Path("src/ps/ps_phone_pairing_server.h").read_text()
    source = Path("src/ps/ps_phone_pairing_server.cpp").read_text()
    activity_h = Path("src/ui/ps_registration_activity.h").read_text()
    activity = Path("src/ui/ps_registration_activity.cpp").read_text()

    assert "struct PsPhonePairingInput" in header
    assert "std::string account_id" in header
    assert "uint32_t pin" in header
    assert "decimalPsnAccountIdToBase64(account_input, account_id)" in source
    assert "normalizeBase64PsnAccountId(account_input, account_id)" in source
    assert "pin_text.size() == 8" in source
    assert 'name=\\"account_input\\"' in source
    assert 'name=\\"account_type\\"' in source
    assert 'value=\\"username\\"' in source
    assert 'value=\\"decimal_id\\"' in source
    assert 'value=\\"base64_id\\"' in source
    assert 'account_type == "username"' in source
    assert 'account_type == "decimal_id"' in source
    assert 'account_type == "base64_id"' in source
    assert "lookupPsnAccountId(account_input, account_id, lookup_error)" in source
    assert "third-party FlipScreen" in source
    assert 'name=\\"pin\\"' in source
    assert 'type=\\"password\\"' in source
    assert "sessionToken" in source
    assert "PsPhonePairingServer" in activity_h
    assert "manager_->getPairingAccountId().empty()" in activity
    assert "makeQrCode(phone_pairing_server_.getHelperUrl())" in activity
    assert "saveManualPairingAccountId(input.account_id)" in activity
    assert "pin_buffer_ = std::to_string(input.pin)" in activity
    assert "onSubmitPin();" in activity
    print("PS phone pairing tests passed")


if __name__ == "__main__":
    main()
