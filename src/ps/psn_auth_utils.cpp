#include "psn_auth_utils.h"

#include <cctype>

namespace lunar::ps {
namespace {

constexpr char kBase64Alphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

int hexValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

std::string trim(const std::string& input) {
    size_t begin = 0;
    while (begin < input.size() && std::isspace(static_cast<unsigned char>(input[begin]))) {
        ++begin;
    }
    size_t end = input.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(input[end - 1]))) {
        --end;
    }
    return input.substr(begin, end - begin);
}

bool urlDecode(const std::string& input, std::string& output) {
    output.clear();
    output.reserve(input.size());
    for (size_t i = 0; i < input.size(); ++i) {
        char c = input[i];
        if (c == '+') {
            output.push_back(' ');
            continue;
        }
        if (c != '%') {
            output.push_back(c);
            continue;
        }
        if (i + 2 >= input.size()) return false;
        int high = hexValue(input[i + 1]);
        int low = hexValue(input[i + 2]);
        if (high < 0 || low < 0) return false;
        output.push_back(static_cast<char>((high << 4) | low));
        i += 2;
    }
    return true;
}

} // namespace

std::string base64Encode(const uint8_t* data, size_t size) {
    std::string output;
    output.reserve(((size + 2) / 3) * 4);
    uint32_t value = 0;
    int bits = -6;
    for (size_t i = 0; i < size; ++i) {
        value = (value << 8) | data[i];
        bits += 8;
        while (bits >= 0) {
            output.push_back(kBase64Alphabet[(value >> bits) & 0x3f]);
            bits -= 6;
        }
    }
    if (bits > -6) output.push_back(kBase64Alphabet[((value << 8) >> (bits + 8)) & 0x3f]);
    while (output.size() % 4 != 0) output.push_back('=');
    return output;
}

bool base64Decode(const std::string& input, std::string& output) {
    output.clear();
    uint32_t value = 0;
    int bits = -8;
    for (char c : input) {
        if (c == '=') break;
        const char* found = nullptr;
        for (const char* p = kBase64Alphabet; *p; ++p) {
            if (*p == c) {
                found = p;
                break;
            }
        }
        if (!found) return false;
        value = (value << 6) | static_cast<uint32_t>(found - kBase64Alphabet);
        bits += 6;
        if (bits >= 0) {
            output.push_back(static_cast<char>((value >> bits) & 0xff));
            bits -= 8;
        }
    }
    return !output.empty();
}

std::string extractPsnAuthorizationCode(const std::string& input) {
    std::string value = trim(input);
    if (value.empty()) return {};

    size_t query = value.find('?');
    if (query == std::string::npos) {
        return value.find("://") == std::string::npos ? value : std::string{};
    }

    size_t cursor = query + 1;
    while (cursor <= value.size()) {
        size_t end = value.find_first_of("&#", cursor);
        if (end == std::string::npos) end = value.size();
        size_t equals = value.find('=', cursor);
        if (equals != std::string::npos && equals < end &&
            value.compare(cursor, equals - cursor, "code") == 0) {
            std::string decoded;
            if (urlDecode(value.substr(equals + 1, end - equals - 1), decoded)) {
                return decoded;
            }
            return {};
        }
        if (end == value.size() || value[end] == '#') break;
        cursor = end + 1;
    }
    return {};
}

} // namespace lunar::ps
