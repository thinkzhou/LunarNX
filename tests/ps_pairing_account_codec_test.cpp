#include "../src/ps/ps_pairing_account.h"
#include "../src/ps/psn_auth_utils.h"

#include <cassert>
#include <string>

int main() {
    std::string encoded;

    // Apollo displays the numeric account ID as 16 hexadecimal digits. Sony's
    // registration payload expects those uint64 bytes in little-endian order.
    assert(lunar::ps::hexPsnAccountIdToBase64("0123456789ABCDEF", encoded));
    std::string decoded;
    assert(lunar::ps::base64Decode(encoded, decoded));
    assert(decoded == std::string("\xef\xcd\xab\x89\x67\x45\x23\x01", 8));

    assert(lunar::ps::normalizePsnAccountId("0x0123456789abcdef", encoded));
    assert(lunar::ps::base64Decode(encoded, decoded));
    assert(decoded == std::string("\xef\xcd\xab\x89\x67\x45\x23\x01", 8));

    assert(!lunar::ps::hexPsnAccountIdToBase64("1234", encoded));
    assert(!lunar::ps::hexPsnAccountIdToBase64("0123456789abcdeg", encoded));

    assert(lunar::ps::decimalPsnAccountIdToBase64("18446744073709551615", encoded));
    assert(lunar::ps::base64Decode(encoded, decoded));
    assert(decoded == std::string(8, static_cast<char>(0xff)));
    assert(!lunar::ps::decimalPsnAccountIdToBase64("18446744073709551616", encoded));
    assert(!lunar::ps::decimalPsnAccountIdToBase64("12x", encoded));

    // Explicit format selection keeps a numeric Apollo value from being
    // interpreted as decimal by the phone pairing form.
    assert(lunar::ps::hexPsnAccountIdToBase64("0000000000000010", encoded));
    assert(lunar::ps::base64Decode(encoded, decoded));
    assert(decoded == std::string("\x10\0\0\0\0\0\0\0", 8));
    assert(!lunar::ps::normalizeBase64PsnAccountId("AQI=", encoded));
    return 0;
}
