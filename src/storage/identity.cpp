#include "storage/identity.h"

#include <sodium.h>

#include "util/fs.h"

namespace wfw::storage {

DeviceIdentity::DeviceIdentity(std::string dataDir)
    : path_(fs::joinPath(dataDir, "identity.key")) {}

bool DeviceIdentity::loadOrCreate(std::string& err) {
  std::string text;
  bool missing = false;
  crypto::Secret<32> seed;
  if (fs::readFile(path_, text, err, &missing)) {
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.pop_back();
    auto bytes = hexDecode(text);
    sodium_memzero(text.data(), text.size());
    if (!bytes || bytes->size() != 32) {
      err = "identity.key is corrupt";
      return false;
    }
    seed.copyFrom(bytes->data());
    sodium_memzero(bytes->data(), bytes->size());
  } else if (missing) {
    randombytes_buf(seed.data(), seed.size());
    std::string hex = hexEncode(seed.data(), seed.size()) + "\n";
    const bool ok = fs::writeFileAtomic(path_, hex, 0600, err);
    sodium_memzero(hex.data(), hex.size());
    if (!ok) return false;
  } else {
    return false;
  }
  crypto::keyPairFromSeed(seed.data(), keyPair_);
  ready_ = true;
  return true;
}

void DeviceIdentity::setSeedForTest(const uint8_t seed[32]) {
  crypto::keyPairFromSeed(seed, keyPair_);
  ready_ = true;
}

std::string DeviceIdentity::sign(const std::string& message) const {
  uint8_t sig[crypto::kSignatureBytes];
  crypto::signDetached(keyPair_, reinterpret_cast<const uint8_t*>(message.data()), message.size(),
                       sig);
  return base64Encode(sig, sizeof(sig));
}

}  // namespace wfw::storage
