#include "storage/keystore.h"

#include <sodium.h>

#include <nlohmann/json.hpp>

#include "util/fs.h"

namespace wfw::storage {

using nlohmann::json;

namespace {

constexpr const char* kFormat = "wallet-fw/keystore";
constexpr int kVersion = 1;
constexpr const char* kSchemePlain = "plain";
constexpr const char* kSchemePin = "argon2id13+xsalsa20poly1305";

}  // namespace

Keystore::Keystore(std::string dataDir)
    : path_(fs::joinPath(dataDir, "wallet.json")),
      opsLimit_(crypto_pwhash_OPSLIMIT_INTERACTIVE),
      memLimit_(crypto_pwhash_MEMLIMIT_INTERACTIVE) {}

void Keystore::setKdfLimits(unsigned long long opsLimit, size_t memLimit) {
  opsLimit_ = opsLimit;
  memLimit_ = memLimit;
}

std::string Keystore::address() const { return base58Encode(publicKey_); }

bool Keystore::adoptEntropy(const uint8_t* entropy, std::string& err) {
  crypto::Mnemonic m;
  if (!m.fromEntropy(entropy, crypto::kEntropyBytes)) {
    err = "invalid entropy";
    return false;
  }
  crypto::KeyPair kp;
  crypto::deriveSolanaKeyPair(m, kp);
  if (initialized() && kp.publicKey != publicKey_) {
    err = "keystore integrity check failed (public key mismatch)";
    return false;
  }
  entropy_.copyFrom(entropy);
  keyPair_.publicKey = kp.publicKey;
  keyPair_.secretKey.copyFrom(kp.secretKey);
  publicKey_ = kp.publicKey;
  return true;
}

bool Keystore::load(std::string& err) {
  auto fail = [&](const std::string& why) {
    status_ = Status::Error;
    lastError_ = err = why;
    return false;
  };

  std::string text;
  bool missing = false;
  if (!fs::readFile(path_, text, err, &missing)) {
    if (missing) {
      status_ = Status::Empty;
      return true;
    }
    return fail(err);
  }
  json doc = json::parse(text, nullptr, false);
  sodium_memzero(text.data(), text.size());
  if (doc.is_discarded() || !doc.is_object()) return fail("keystore is not valid JSON");

  try {
    if (doc.value("format", "") != kFormat || doc.value("version", 0) != kVersion) {
      return fail("unsupported keystore format");
    }
    auto pub = decodeAddress(doc.at("pubkey").get<std::string>());
    if (!pub) return fail("keystore public key is invalid");
    backupConfirmed_ = doc.value("backup_confirmed", false);
    const json& secret = doc.at("secret");
    const std::string scheme = secret.at("scheme").get<std::string>();

    if (scheme == kSchemePlain) {
      std::string hex = secret.at("entropy").get<std::string>();
      auto entropy = hexDecode(hex);
      sodium_memzero(hex.data(), hex.size());
      if (!entropy || entropy->size() != crypto::kEntropyBytes) {
        return fail("keystore entropy is invalid");
      }
      publicKey_ = *pub;
      status_ = Status::Locked;  // so adoptEntropy checks against the stored key
      pinProtected_ = false;
      const bool ok = adoptEntropy(entropy->data(), err);
      sodium_memzero(entropy->data(), entropy->size());
      if (!ok) return fail(err);
      status_ = Status::Ready;
    } else if (scheme == kSchemePin) {
      auto salt = base64Decode(secret.at("salt").get<std::string>());
      auto nonce = base64Decode(secret.at("nonce").get<std::string>());
      auto ct = base64Decode(secret.at("ciphertext").get<std::string>());
      if (!salt || salt->size() != crypto_pwhash_SALTBYTES || !nonce ||
          nonce->size() != crypto_secretbox_NONCEBYTES || !ct ||
          ct->size() != crypto::kEntropyBytes + crypto_secretbox_MACBYTES) {
        return fail("keystore ciphertext is invalid");
      }
      salt_ = *salt;
      nonce_ = *nonce;
      ciphertext_ = *ct;
      fileOpsLimit_ = secret.at("opslimit").get<unsigned long long>();
      fileMemLimit_ = secret.at("memlimit").get<size_t>();
      publicKey_ = *pub;
      pinProtected_ = true;
      status_ = Status::Locked;
    } else {
      return fail("unknown keystore scheme: " + scheme);
    }
  } catch (const json::exception& e) {
    return fail(std::string("keystore field error: ") + e.what());
  }
  lastError_.clear();
  return true;
}

bool Keystore::persist(std::string& err) {
  json secret;
  if (pinProtected_) {
    secret = {{"scheme", kSchemePin},
              {"opslimit", fileOpsLimit_},
              {"memlimit", fileMemLimit_},
              {"salt", base64Encode(salt_)},
              {"nonce", base64Encode(nonce_)},
              {"ciphertext", base64Encode(ciphertext_)}};
  } else {
    if (!ready()) {
      err = "keystore not loaded";
      return false;
    }
    secret = {{"scheme", kSchemePlain},
              {"entropy", hexEncode(entropy_.data(), entropy_.size())}};
  }
  json doc = {{"format", kFormat},
              {"version", kVersion},
              {"pubkey", address()},
              {"backup_confirmed", backupConfirmed_},
              {"secret", secret}};
  std::string text = doc.dump(2) + "\n";
  const bool ok = fs::writeFileAtomic(path_, text, 0600, err);
  sodium_memzero(text.data(), text.size());
  return ok;
}

bool Keystore::create(const uint8_t entropy[crypto::kEntropyBytes], const std::string& pin,
                      std::string& err) {
  if (initialized()) {
    err = "already initialized";
    return false;
  }
  status_ = Status::Empty;
  if (!adoptEntropy(entropy, err)) return false;
  backupConfirmed_ = false;
  pinProtected_ = !pin.empty();

  if (pinProtected_) {
    salt_.resize(crypto_pwhash_SALTBYTES);
    nonce_.resize(crypto_secretbox_NONCEBYTES);
    randombytes_buf(salt_.data(), salt_.size());
    randombytes_buf(nonce_.data(), nonce_.size());
    fileOpsLimit_ = opsLimit_;
    fileMemLimit_ = memLimit_;
    crypto::Secret<crypto_secretbox_KEYBYTES> key;
    if (crypto_pwhash(key.data(), key.size(), pin.data(), pin.size(), salt_.data(), fileOpsLimit_,
                      fileMemLimit_, crypto_pwhash_ALG_ARGON2ID13) != 0) {
      err = "Argon2id failed (out of memory?)";
      lock();
      status_ = Status::Empty;
      return false;
    }
    ciphertext_.resize(crypto::kEntropyBytes + crypto_secretbox_MACBYTES);
    crypto_secretbox_easy(ciphertext_.data(), entropy_.data(), entropy_.size(), nonce_.data(),
                          key.data());
  }
  status_ = Status::Ready;
  if (!persist(err)) {
    lock();
    status_ = Status::Empty;
    return false;
  }
  return true;
}

bool Keystore::unlock(const std::string& pin, std::string& err) {
  if (status_ == Status::Ready) return true;
  if (status_ != Status::Locked || !pinProtected_) {
    err = "keystore not locked";
    return false;
  }
  crypto::Secret<crypto_secretbox_KEYBYTES> key;
  if (crypto_pwhash(key.data(), key.size(), pin.data(), pin.size(), salt_.data(), fileOpsLimit_,
                    fileMemLimit_, crypto_pwhash_ALG_ARGON2ID13) != 0) {
    err = "Argon2id failed (out of memory?)";
    return false;
  }
  crypto::Secret<crypto::kEntropyBytes> entropy;
  if (crypto_secretbox_open_easy(entropy.data(), ciphertext_.data(), ciphertext_.size(),
                                 nonce_.data(), key.data()) != 0) {
    err = "wrong PIN";
    return false;
  }
  if (!adoptEntropy(entropy.data(), err)) return false;
  status_ = Status::Ready;
  return true;
}

void Keystore::lock() {
  entropy_.wipe();
  keyPair_.secretKey.wipe();
  if (status_ == Status::Ready && pinProtected_) status_ = Status::Locked;
}

bool Keystore::setBackupConfirmed(std::string& err) {
  if (!initialized()) {
    err = "not initialized";
    return false;
  }
  if (!pinProtected_ && !ready()) {
    err = "keystore not loaded";
    return false;
  }
  const bool previous = backupConfirmed_;
  backupConfirmed_ = true;
  if (!persist(err)) {
    backupConfirmed_ = previous;
    return false;
  }
  return true;
}

bool Keystore::wipe(std::string& err) {
  if (!fs::removeFile(path_, err)) return false;
  entropy_.wipe();
  keyPair_.secretKey.wipe();
  keyPair_.publicKey = {};
  publicKey_ = {};
  backupConfirmed_ = false;
  pinProtected_ = false;
  salt_.clear();
  nonce_.clear();
  ciphertext_.clear();
  status_ = Status::Empty;
  return true;
}

bool Keystore::mnemonic(crypto::Mnemonic& out) const {
  if (!ready()) return false;
  return out.fromEntropy(entropy_.data(), entropy_.size());
}

}  // namespace wfw::storage
