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
    return 0;
}
