#pragma once

#include <string>

#include "crypto/keys.h"

namespace wfw::storage {

// Aurora's device identity: an Ed25519 key created on first boot and kept in
// <data_dir>/identity.key (0600). It is independent of the wallet seed, so the
// app can recognise the device before setup and across wallet resets. It
// signs ping and pairing replies; it never signs transactions.
class DeviceIdentity {
 public:
  explicit DeviceIdentity(std::string dataDir);

  // Loads the key, creating it if absent.
  bool loadOrCreate(std::string& err);
  bool ready() const { return ready_; }
  const PubKey& publicKey() const { return keyPair_.publicKey; }
  std::string deviceId() const { return base58Encode(keyPair_.publicKey); }
  std::string sign(const std::string& message) const;  // base64 signature
  const crypto::KeyPair& keyPair() const { return keyPair_; }
  // For tests: use a fixed seed instead of the file.
  void setSeedForTest(const uint8_t seed[32]);

 private:
  std::string path_;
  bool ready_ = false;
  crypto::KeyPair keyPair_;
};

}  // namespace wfw::storage
