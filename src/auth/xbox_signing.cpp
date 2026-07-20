#include "xbox_signing.h"
#include <cstdio>
#include <cstring>
#include <vector>
#include <chrono>

#ifdef __SWITCH__
#include <mbedtls/ecdsa.h>
#include <mbedtls/ecp.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/sha256.h>
#include <mbedtls/bignum.h>
#include <mbedtls/base64.h>
#include <mbedtls/version.h>
#ifndef MBEDTLS_PRIVATE
#define MBEDTLS_PRIVATE(member) member
#endif
#else
#include <openssl/evp.h>
#include <openssl/ec.h>
#include <openssl/pem.h>
#include <openssl/bn.h>
#include <openssl/sha.h>
#include <openssl/err.h>
#endif

namespace lunar::auth {

// =============================================================================
// Windows timestamp (100-nanosecond intervals since 1601-01-01)
// =============================================================================
static uint64_t windows_timestamp_now() {
    static constexpr uint64_t EPOCH_DIFF = 11644473600ULL * 10000000ULL;
    auto now = std::chrono::system_clock::now();
    auto usecs = std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count();
    return static_cast<uint64_t>(usecs) * 10ULL + EPOCH_DIFF;
}

// =============================================================================
// Base64 encoding (standard)
// =============================================================================
static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static std::string base64_encode(const uint8_t* data, size_t len) {
    std::string r;
    r.reserve(((len + 2) / 3) * 4);
    for (size_t i = 0; i < len; i += 3) {
        uint32_t n = static_cast<uint32_t>(data[i]) << 16;
        if (i + 1 < len) n |= static_cast<uint32_t>(data[i + 1]) << 8;
        if (i + 2 < len) n |= static_cast<uint32_t>(data[i + 2]);
        r.push_back(B64[(n >> 18) & 0x3F]);
        r.push_back(B64[(n >> 12) & 0x3F]);
        r.push_back((i + 1 < len) ? B64[(n >> 6) & 0x3F] : '=');
        r.push_back((i + 2 < len) ? B64[n & 0x3F] : '=');
    }
    return r;
}

static std::string base64url_encode(const uint8_t* data, size_t len) {
    std::string b64 = base64_encode(data, len);
    while (!b64.empty() && b64.back() == '=') b64.pop_back();
    for (auto& c : b64) {
        if (c == '+') c = '-';
        else if (c == '/') c = '_';
    }
    return b64;
}

// =============================================================================
// XboxSigning
// =============================================================================

#ifdef __SWITCH__
// ---- mbedtls implementation ----

struct MbedTlsRng {
    mbedtls_ctr_drbg_context ctr_drbg;
    mbedtls_entropy_context entropy;
    bool seeded = false;

    MbedTlsRng() {
        mbedtls_ctr_drbg_init(&ctr_drbg);
        mbedtls_entropy_init(&entropy);
        seeded = mbedtls_ctr_drbg_seed(
            &ctr_drbg, mbedtls_entropy_func, &entropy, nullptr, 0) == 0;
    }

    ~MbedTlsRng() {
        mbedtls_ctr_drbg_free(&ctr_drbg);
        mbedtls_entropy_free(&entropy);
    }

    static int random(void* ctx, unsigned char* buf, size_t len) {
        return mbedtls_ctr_drbg_random(ctx, buf, len);
    }
};

static int mbedtls_sha256_compat(const unsigned char* input,
                                 size_t ilen,
                                 unsigned char output[32]) {
#if defined(MBEDTLS_VERSION_MAJOR) && MBEDTLS_VERSION_MAJOR >= 3
    return mbedtls_sha256(input, ilen, output, 0);
#else
    return mbedtls_sha256_ret(input, ilen, output, 0);
#endif
}

XboxSigning::XboxSigning() { generate_keys(); }
XboxSigning::~XboxSigning() = default;

void XboxSigning::generate_keys() {
    MbedTlsRng rng;
    if (!rng.seeded) {
        fprintf(stderr, "[sign] rng seed failed\n");
        return;
    }

    mbedtls_ecp_group grp;
    mbedtls_ecp_group_init(&grp);
    mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_SECP256R1);

    mbedtls_mpi d;
    mbedtls_mpi_init(&d);
    mbedtls_ecp_point Q;
    mbedtls_ecp_point_init(&Q);

    int ret = mbedtls_ecp_gen_keypair(&grp, &d, &Q,
        MbedTlsRng::random, &rng.ctr_drbg);
    if (ret != 0) {
        fprintf(stderr, "[sign] keygen failed: %d\n", ret);
        return;
    }

    // Extract coordinates
    mbedtls_mpi_write_binary(&d, private_key_, 32);
    mbedtls_mpi_write_binary(&Q.MBEDTLS_PRIVATE(X), public_key_x_, 32);
    mbedtls_mpi_write_binary(&Q.MBEDTLS_PRIVATE(Y), public_key_y_, 32);

    mbedtls_mpi_free(&d);
    mbedtls_ecp_point_free(&Q);
    mbedtls_ecp_group_free(&grp);

    public_key_x_b64_ = base64url_encode(public_key_x_, 32);
    public_key_y_b64_ = base64url_encode(public_key_y_, 32);
    private_key_d_b64_ = base64url_encode(private_key_, 32);
}

std::string XboxSigning::signRequest(const std::string& method,
                                      const std::string& url,
                                      const std::string& auth_token,
                                      const std::string& payload) {
    MbedTlsRng rng;
    if (!rng.seeded) {
        fprintf(stderr, "[sign] rng seed failed\n");
        return "";
    }

    uint64_t win_ts = windows_timestamp_now();

    // Parse path
    size_t scheme_end = url.find("://");
    size_t host_start = (scheme_end != std::string::npos) ? scheme_end + 3 : 0;
    size_t path_start = url.find('/', host_start);
    std::string path = (path_start != std::string::npos) ? url.substr(path_start) : "/";

    // Build signature buffer: version(4B) + 0 + timestamp(8B) + 0 + method + 0 + path + 0 + auth + 0 + payload + 0
    size_t sz = 4 + 1 + 8 + 1 + method.size() + 1 + path.size() + 1 + auth_token.size() + 1 + payload.size() + 1;
    std::vector<uint8_t> buf(sz);
    size_t off = 0;
    buf[off++] = 0; buf[off++] = 0; buf[off++] = 0; buf[off++] = 1;  // version
    buf[off++] = 0;
    for (int i = 7; i >= 0; i--) buf[off++] = static_cast<uint8_t>((win_ts >> (i * 8)) & 0xFF);
    buf[off++] = 0;
    std::memcpy(&buf[off], method.data(), method.size()); off += method.size(); buf[off++] = 0;
    std::memcpy(&buf[off], path.data(), path.size()); off += path.size(); buf[off++] = 0;
    std::memcpy(&buf[off], auth_token.data(), auth_token.size()); off += auth_token.size(); buf[off++] = 0;
    std::memcpy(&buf[off], payload.data(), payload.size()); off += payload.size(); buf[off++] = 0;

    // SHA-256 hash
    uint8_t hash[32];
    if (mbedtls_sha256_compat(buf.data(), buf.size(), hash) != 0) {
        return "";
    }

    // ECDSA sign
    mbedtls_mpi r, s, d;
    mbedtls_mpi_init(&r); mbedtls_mpi_init(&s); mbedtls_mpi_init(&d);
    mbedtls_mpi_read_binary(&d, private_key_, 32);

    mbedtls_ecp_group grp;
    mbedtls_ecp_group_init(&grp);
    mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_SECP256R1);

    int ret = mbedtls_ecdsa_sign_det_ext(&grp, &r, &s, &d, hash, 32, MBEDTLS_MD_SHA256,
        MbedTlsRng::random, &rng.ctr_drbg);
    if (ret != 0) {
        fprintf(stderr, "[sign] ecdsa failed: %d\n", ret);
        mbedtls_mpi_free(&r); mbedtls_mpi_free(&s); mbedtls_mpi_free(&d);
        mbedtls_ecp_group_free(&grp);
        return "";
    }

    // IEEE P1363 format: r||s (64 bytes)
    std::vector<uint8_t> raw_sig(64);
    mbedtls_mpi_write_binary(&r, raw_sig.data(), 32);
    mbedtls_mpi_write_binary(&s, raw_sig.data() + 32, 32);

    mbedtls_mpi_free(&r); mbedtls_mpi_free(&s); mbedtls_mpi_free(&d);
    mbedtls_ecp_group_free(&grp);

    // Build header: version(4B) + timestamp(8B) + signature(64B)
    std::vector<uint8_t> header(4 + 8 + 64);
    size_t hoff = 0;
    header[hoff++] = 0; header[hoff++] = 0; header[hoff++] = 0; header[hoff++] = 1;
    for (int i = 7; i >= 0; i--) header[hoff++] = static_cast<uint8_t>((win_ts >> (i * 8)) & 0xFF);
    std::memcpy(&header[hoff], raw_sig.data(), 64);

    return base64_encode(header.data(), header.size());
}

std::string XboxSigning::getPublicKeyX() const { return public_key_x_b64_; }
std::string XboxSigning::getPublicKeyY() const { return public_key_y_b64_; }
std::string XboxSigning::getPrivateKeyD() const { return private_key_d_b64_; }

#else
// ---- OpenSSL implementation (desktop) ----
XboxSigning::XboxSigning() { generate_keys(); }

XboxSigning::~XboxSigning() {
    if (ec_key_) EVP_PKEY_free(static_cast<EVP_PKEY*>(ec_key_));
}

void XboxSigning::generate_keys() {
    EVP_PKEY_CTX* pctx = EVP_PKEY_CTX_new_id(EVP_PKEY_EC, nullptr);
    EVP_PKEY* pkey = nullptr;
    EVP_PKEY_keygen_init(pctx);
    EVP_PKEY_CTX_set_ec_paramgen_curve_nid(pctx, NID_X9_62_prime256v1);
    EVP_PKEY_keygen(pctx, &pkey);
    EVP_PKEY_CTX_free(pctx);
    ec_key_ = pkey;

    EC_KEY* ec = EVP_PKEY_get1_EC_KEY(pkey);
    const EC_POINT* pub = EC_KEY_get0_public_key(ec);
    const BIGNUM* priv = EC_KEY_get0_private_key(ec);
    BN_CTX* bn_ctx = BN_CTX_new();
    BIGNUM* x = BN_new(); BIGNUM* y = BN_new();

    const EC_GROUP* group = EC_KEY_get0_group(ec);
    EC_POINT_get_affine_coordinates(group, pub, x, y, bn_ctx);

    uint8_t xb[32] = {}, yb[32] = {}, db[32] = {};
    BN_bn2binpad(x, xb, 32); BN_bn2binpad(y, yb, 32); BN_bn2binpad(priv, db, 32);

    public_key_x_b64_ = base64url_encode(xb, 32);
    public_key_y_b64_ = base64url_encode(yb, 32);
    private_key_d_b64_ = base64url_encode(db, 32);

    BN_free(x); BN_free(y); BN_CTX_free(bn_ctx); EC_KEY_free(ec);
}

std::string XboxSigning::signRequest(const std::string& method,
                                      const std::string& url,
                                      const std::string& auth_token,
                                      const std::string& payload) {
    uint64_t win_ts = windows_timestamp_now();
    size_t scheme_end = url.find("://");
    size_t host_start = (scheme_end != std::string::npos) ? scheme_end + 3 : 0;
    size_t path_start = url.find('/', host_start);
    std::string path = (path_start != std::string::npos) ? url.substr(path_start) : "/";

    size_t sz = 4 + 1 + 8 + 1 + method.size() + 1 + path.size() + 1 + auth_token.size() + 1 + payload.size() + 1;
    std::vector<uint8_t> buf(sz);
    size_t off = 0;
    buf[off++] = 0; buf[off++] = 0; buf[off++] = 0; buf[off++] = 1;
    buf[off++] = 0;
    for (int i = 7; i >= 0; i--) buf[off++] = static_cast<uint8_t>((win_ts >> (i * 8)) & 0xFF);
    buf[off++] = 0;
    std::memcpy(&buf[off], method.data(), method.size()); off += method.size(); buf[off++] = 0;
    std::memcpy(&buf[off], path.data(), path.size()); off += path.size(); buf[off++] = 0;
    std::memcpy(&buf[off], auth_token.data(), auth_token.size()); off += auth_token.size(); buf[off++] = 0;
    std::memcpy(&buf[off], payload.data(), payload.size()); off += payload.size(); buf[off++] = 0;

    EVP_MD_CTX* md_ctx = EVP_MD_CTX_new();
    EVP_DigestSignInit(md_ctx, nullptr, EVP_sha256(), nullptr, static_cast<EVP_PKEY*>(ec_key_));
    size_t sig_len = 0;
    EVP_DigestSign(md_ctx, nullptr, &sig_len, buf.data(), buf.size());
    std::vector<uint8_t> sig(sig_len);
    EVP_DigestSign(md_ctx, sig.data(), &sig_len, buf.data(), buf.size());
    EVP_MD_CTX_free(md_ctx);

    const uint8_t* der = sig.data();
    ECDSA_SIG* ecdsa_sig = d2i_ECDSA_SIG(nullptr, &der, static_cast<long>(sig_len));
    const BIGNUM* sig_r = ECDSA_SIG_get0_r(ecdsa_sig);
    const BIGNUM* sig_s = ECDSA_SIG_get0_s(ecdsa_sig);
    std::vector<uint8_t> raw(64);
    BN_bn2binpad(sig_r, raw.data(), 32);
    BN_bn2binpad(sig_s, raw.data() + 32, 32);
    ECDSA_SIG_free(ecdsa_sig);

    std::vector<uint8_t> header(4 + 8 + 64);
    header[0] = 0; header[1] = 0; header[2] = 0; header[3] = 1;
    for (int i = 7; i >= 0; i--) header[4 + (7-i)] = static_cast<uint8_t>((win_ts >> (i * 8)) & 0xFF);
    std::memcpy(&header[12], raw.data(), 64);
    return base64_encode(header.data(), header.size());
}

std::string XboxSigning::getPublicKeyX() const { return public_key_x_b64_; }
std::string XboxSigning::getPublicKeyY() const { return public_key_y_b64_; }
std::string XboxSigning::getPrivateKeyD() const { return private_key_d_b64_; }
#endif

} // namespace lunar::auth
