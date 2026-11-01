// PBKDF2-HMAC-SHA256 on top of PSA hashes (mbedTLS 4 / ESP-IDF 6 removed the
// legacy md/pkcs5 APIs). HMAC inner/outer states are precomputed once and
// cloned per iteration.
#include "password.h"

#include <string.h>

#include "psa/crypto.h"

namespace cfg {
namespace {

constexpr size_t kBlock = 64;
constexpr size_t kHash = 32;

struct Hmac {
    psa_hash_operation_t inner = PSA_HASH_OPERATION_INIT;
    psa_hash_operation_t outer = PSA_HASH_OPERATION_INIT;
    ~Hmac() {
        psa_hash_abort(&inner);
        psa_hash_abort(&outer);
    }
    bool init(const uint8_t* key, size_t len) {
        uint8_t k0[kBlock] = {};
        if (len > kBlock) {
            size_t out_len = 0;
            if (psa_hash_compute(PSA_ALG_SHA_256, key, len, k0, kHash, &out_len) != PSA_SUCCESS)
                return false;
        } else {
            memcpy(k0, key, len);
        }
        uint8_t ipad[kBlock], opad[kBlock];
        for (size_t i = 0; i < kBlock; i++) {
            ipad[i] = k0[i] ^ 0x36;
            opad[i] = k0[i] ^ 0x5c;
        }
        bool ok = psa_hash_setup(&inner, PSA_ALG_SHA_256) == PSA_SUCCESS &&
                  psa_hash_update(&inner, ipad, kBlock) == PSA_SUCCESS &&
                  psa_hash_setup(&outer, PSA_ALG_SHA_256) == PSA_SUCCESS &&
                  psa_hash_update(&outer, opad, kBlock) == PSA_SUCCESS;
        memset(k0, 0, sizeof(k0));
        return ok;
    }
    // out = HMAC(key, a || b)
    bool mac(const uint8_t* a, size_t alen, const uint8_t* b, size_t blen, uint8_t out[kHash]) {
        psa_hash_operation_t i = PSA_HASH_OPERATION_INIT, o = PSA_HASH_OPERATION_INIT;
        uint8_t ih[kHash];
        size_t n = 0;
        bool ok = psa_hash_clone(&inner, &i) == PSA_SUCCESS &&
                  psa_hash_update(&i, a, alen) == PSA_SUCCESS &&
                  (blen == 0 || psa_hash_update(&i, b, blen) == PSA_SUCCESS) &&
                  psa_hash_finish(&i, ih, kHash, &n) == PSA_SUCCESS &&
                  psa_hash_clone(&outer, &o) == PSA_SUCCESS &&
                  psa_hash_update(&o, ih, kHash) == PSA_SUCCESS &&
                  psa_hash_finish(&o, out, kHash, &n) == PSA_SUCCESS;
        psa_hash_abort(&i);
        psa_hash_abort(&o);
        return ok;
    }
};

}  // namespace

bool pbkdf2_sha256(const uint8_t* pw, size_t pw_len, const uint8_t* salt, size_t salt_len,
                   uint32_t iterations, uint8_t out[32]) {
    if (iterations == 0) return false;
    if (psa_crypto_init() != PSA_SUCCESS) return false;
    Hmac h;
    if (!h.init(pw, pw_len)) return false;
    const uint8_t block_index[4] = {0, 0, 0, 1};
    uint8_t u[kHash], t[kHash];
    if (!h.mac(salt, salt_len, block_index, 4, u)) return false;
    memcpy(t, u, kHash);
    for (uint32_t it = 1; it < iterations; it++) {
        if (!h.mac(u, kHash, nullptr, 0, u)) return false;
        for (size_t j = 0; j < kHash; j++) t[j] ^= u[j];
    }
    memcpy(out, t, kHash);
    return true;
}

}  // namespace cfg
