#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace lunar::ps {

std::string base64Encode(const uint8_t* data, size_t size);
bool base64Decode(const std::string& input, std::string& output);

// Accepts either Sony's full redirect URL or a raw authorization code.
std::string extractPsnAuthorizationCode(const std::string& input);

} // namespace lunar::ps
