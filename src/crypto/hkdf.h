#pragma once

#include <cstddef>
#include <cstdint>

namespace wfw::crypto {

constexpr size_t kHkdfSha256HashLen = 32;

// RFC 5869 HKDF with HMAC-SHA256. Implemented on libsodium's HMAC because the
// libsodium shipped by Pi OS Bookworm (1.0.18) predates crypto_kdf_hkdf_sha256.
void hkdfSha256Extract(const uint8_t* salt, size_t saltLen, const uint8_t* ikm, size_t ikmLen,
                       uint8_t prk[kHkdfSha256HashLen]);
// outLen must be <= 255 * 32.
bool hkdfSha256Expand(const uint8_t prk[kHkdfSha256HashLen], const uint8_t* info, size_t infoLen,
                      uint8_t* out, size_t outLen);
bool hkdfSha256(const uint8_t* salt, size_t saltLen, const uint8_t* ikm, size_t ikmLen,
                const uint8_t* info, size_t infoLen, uint8_t* out, size_t outLen);

}  // namespace wfw::crypto
