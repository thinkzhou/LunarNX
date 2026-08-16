#include "ps_pairing_account.h"
#include "psn_auth_utils.h"

#include <algorithm>
#include <cctype>
#include <cstdint>

namespace lunar::ps {
namespace {

std::string trimAccountInput(const std::string& input) {
    const auto first = std::find_if_not(input.begin(), input.end(), [](unsigned char c) {
        return std::isspace(c) != 0;
    });
    const auto last = std::find_if_not(input.rbegin(), input.rend(), [](unsigned char c) {
        return std::isspace(c) != 0;
    }).base();
    return first < last ? std::string(first, last) : std::string();
}

int accountHexValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

} // namespace

bool isValidPsnAccountId(const std::string& account_id) {
    std::string decoded;
    return base64Decode(account_id, decoded) && decoded.size() == 8;
}

bool normalizeBase64PsnAccountId(const std::string& input, std::string& account_id) {
    const std::string value = trimAccountInput(input);
    if (!isValidPsnAccountId(value)) return false;
    account_id = value;
    return true;
}

bool decimalPsnAccountIdToBase64(const std::string& input, std::string& account_id) {
    const std::string value = trimAccountInput(input);
    if (value.empty() || !std::all_of(value.begin(), value.end(), [](unsigned char c) {
            return std::isdigit(c) != 0;
        })) {
        return false;
    }
    try {
        size_t consumed = 0;
        const unsigned long long uid = std::stoull(value, &consumed, 10);
        if (consumed != value.size()) return false;
        uint8_t bytes[8]{};
        for (size_t i = 0; i < sizeof(bytes); ++i) {
            bytes[i] = static_cast<uint8_t>((uid >> (i * 8)) & 0xff);
        }
        account_id = base64Encode(bytes, sizeof(bytes));
        return isValidPsnAccountId(account_id);
    } catch (...) {
        return false;
    }
}

bool hexPsnAccountIdToBase64(const std::string& input, std::string& account_id) {
    std::string value = trimAccountInput(input);
    if (value.size() == 18 && value[0] == '0' &&
        (value[1] == 'x' || value[1] == 'X')) {
        value.erase(0, 2);
    }
    if (value.size() != 16) return false;

    uint8_t bytes[8]{};
    for (size_t i = 0; i < sizeof(bytes); ++i) {
        const size_t source = value.size() - (i + 1) * 2;
        const int high = accountHexValue(value[source]);
        const int low = accountHexValue(value[source + 1]);
        if (high < 0 || low < 0) return false;
        bytes[i] = static_cast<uint8_t>((high << 4) | low);
    }
    account_id = base64Encode(bytes, sizeof(bytes));
    return isValidPsnAccountId(account_id);
}

bool normalizePsnAccountId(const std::string& input, std::string& account_id) {
    return normalizeBase64PsnAccountId(input, account_id) ||
        decimalPsnAccountIdToBase64(input, account_id) ||
        hexPsnAccountIdToBase64(input, account_id);
}

} // namespace lunar::ps
