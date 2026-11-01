#pragma once
#include <stddef.h>
#include <stdint.h>

namespace cfg {
// PBKDF2-HMAC-SHA256, one 32-byte output block. Returns false on failure.
bool pbkdf2_sha256(const uint8_t* pw, size_t pw_len, const uint8_t* salt, size_t salt_len,
                   uint32_t iterations, uint8_t out[32]);
}  // namespace cfg
