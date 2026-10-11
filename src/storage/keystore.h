#pragma once

#include <string>

#include "crypto/keys.h"
#include "crypto/secret.h"

namespace wfw::storage {

// Persists the wallet to <data_dir>/wallet.json (mode 0600).
//
// The stored secret is the 128-bit BIP-39 entropy: it regenerates both the
// 12 words (for the backup check) and the seed. With a PIN it is encrypted
// with crypto_secretbox under an Argon2id (crypto_pwhash) key; the public key
// is always stored in clear so get_pubkey works while the device is locked.
class Keystore {
 public:
  enum class Status { Empty, Locked, Ready, Error };

  explicit Keystore(std::string dataDir);

  // Reads the keystore. A missing file is not an error (status Empty).
  bool load(std::string& err);

  Status status() const { return status_; }
  bool initialized() const { return status_ == Status::Locked || status_ == Status::Ready; }
  bool ready() const { return status_ == Status::Ready; }
  bool pinProtected() const { return pinProtected_; }
  const PubKey& publicKey() const { return publicKey_; }
  std::string address() const;
  // BIP-84 receive address (public; stored so it is known while locked).
  const std::string& bitcoinAddress() const { return bitcoinAddress_; }
  bool backupConfirmed() const { return backupConfirmed_; }
  const std::string& lastError() const { return lastError_; }

  // Creates and persists a new wallet from entropy. Empty pin = no PIN.
  // backupConfirmed: the user already proved the backup on the device.
  bool create(const uint8_t entropy[crypto::kEntropyBytes], const std::string& pin,
              std::string& err, bool backupConfirmed = false);
  // Decrypts with the PIN and checks the derived key against the stored one.
  bool unlock(const std::string& pin, std::string& err);
  // Drops decrypted secrets from memory (PIN-protected wallets only).
  void lock();
  bool setBackupConfirmed(std::string& err);
  bool wipe(std::string& err);
  // Wrong-PIN counter, stored with the wallet so a restart does not reset it.
  int pinFailures() const { return pinFailures_; }
  bool setPinFailures(int count, std::string& err);

  bool allocateReceive(const std::string& chain, uint32_t minimum, uint32_t& index,
                       std::string& address, std::string& err);
  bool receiveIndexAllocated(uint32_t index) const { return index < nextSol_; }
  bool solanaKey(uint32_t index, crypto::KeyPair& out) const;
  bool mnemonic(crypto::Mnemonic& out) const;  // requires ready()
  const crypto::KeyPair* keyPair() const { return ready() ? &keyPair_ : nullptr; }

  // Argon2id cost. Defaults to libsodium's "interactive" limits (~0.5-1 s and
  // 64 MiB on a Pi 4); tests lower them.
  void setKdfLimits(unsigned long long opsLimit, size_t memLimit);

  const std::string& path() const { return path_; }

 private:
  bool persist(std::string& err);
  bool adoptEntropy(const uint8_t* entropy, std::string& err);

  std::string path_;
  Status status_ = Status::Empty;
  std::string lastError_;
  PubKey publicKey_{};
  std::string bitcoinAddress_;
  bool backupConfirmed_ = false;
  bool pinProtected_ = false;
  int pinFailures_ = 0;
  uint32_t nextSol_ = 1;
  uint32_t nextBtc_ = 1;

  // Encrypted blob as stored (kept so metadata updates can rewrite the file
  // without the PIN).
  Bytes salt_;
  Bytes nonce_;
  Bytes ciphertext_;
  unsigned long long opsLimit_;
  size_t memLimit_;
  unsigned long long fileOpsLimit_ = 0;
  size_t fileMemLimit_ = 0;

  crypto::Secret<crypto::kEntropyBytes> entropy_;
  crypto::KeyPair keyPair_;
};

}  // namespace wfw::storage
