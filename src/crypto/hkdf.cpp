#include "crypto/hkdf.h"

#include <sodium.h>

#include <cstring>

namespace wfw::crypto {

void hkdfSha256Extract(const uint8_t* salt, size_t saltLen, const uint8_t* ikm, size_t ikmLen,
                       uint8_t prk[kHkdfSha256HashLen]) {
  static const uint8_t kZeroSalt[kHkdfSha256HashLen] = {0};
  if (salt == nullptr || saltLen == 0) {
    salt = kZeroSalt;
    saltLen = sizeof(kZeroSalt);
  }
  crypto_auth_hmacsha256_state st;
  crypto_auth_hmacsha256_init(&st, salt, saltLen);
  crypto_auth_hmacsha256_update(&st, ikm, ikmLen);
  crypto_auth_hmacsha256_final(&st, prk);
  sodium_memzero(&st, sizeof(st));
}

bool hkdfSha256Expand(const uint8_t prk[kHkdfSha256HashLen], const uint8_t* info, size_t infoLen,
                      uint8_t* out, size_t outLen) {
  if (outLen > 255 * kHkdfSha256HashLen) return false;
  uint8_t t[kHkdfSha256HashLen];
  size_t tLen = 0;
  size_t done = 0;
  for (uint8_t counter = 1; done < outLen; ++counter) {
    crypto_auth_hmacsha256_state st;
    crypto_auth_hmacsha256_init(&st, prk, kHkdfSha256HashLen);
    crypto_auth_hmacsha256_update(&st, t, tLen);
    if (infoLen) crypto_auth_hmacsha256_update(&st, info, infoLen);
    crypto_auth_hmacsha256_update(&st, &counter, 1);
    crypto_auth_hmacsha256_final(&st, t);
    sodium_memzero(&st, sizeof(st));
    tLen = sizeof(t);
    const size_t n = (outLen - done < tLen) ? outLen - done : tLen;
    std::memcpy(out + done, t, n);
    done += n;
  }
  sodium_memzero(t, sizeof(t));
  return true;
}

bool hkdfSha256(const uint8_t* salt, size_t saltLen, const uint8_t* ikm, size_t ikmLen,
                const uint8_t* info, size_t infoLen, uint8_t* out, size_t outLen) {
  uint8_t prk[kHkdfSha256HashLen];
  hkdfSha256Extract(salt, saltLen, ikm, ikmLen, prk);
  const bool ok = hkdfSha256Expand(prk, info, infoLen, out, outLen);
  sodium_memzero(prk, sizeof(prk));
  return ok;
}

}  // namespace wfw::crypto
