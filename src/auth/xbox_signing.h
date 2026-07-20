#pragma once

#include <string>

namespace lunar::auth {

/// ECDSA P-256 signing for Xbox Live Proof-of-Possession.
/// Desktop: OpenSSL EVP. Switch: mbedtls.
class XboxSigning {
public:
    XboxSigning();
    ~XboxSigning();

    XboxSigning(const XboxSigning&) = delete;
    XboxSigning& operator=(const XboxSigning&) = delete;

    std::string signRequest(const std::string& method,
                            const std::string& url,
                            const std::string& authorization_token,
                            const std::string& payload);

    std::string getPublicKeyX() const;
    std::string getPublicKeyY() const;
    std::string getPrivateKeyD() const;

private:
    void generate_keys();

#ifdef __SWITCH__
    unsigned char private_key_[32] = {};
    unsigned char public_key_x_[32] = {};
    unsigned char public_key_y_[32] = {};
#else
    void* ec_key_ = nullptr;  // EVP_PKEY*
#endif
    std::string public_key_x_b64_;
    std::string public_key_y_b64_;
    std::string private_key_d_b64_;
};

} // namespace lunar::auth
