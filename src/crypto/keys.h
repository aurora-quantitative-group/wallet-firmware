#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "crypto/encoding.h"
#include "crypto/secret.h"

namespace wfw::crypto {

constexpr size_t kEntropyBytes = 16;  // 128 bits -> 12 words
constexpr size_t kMnemonicWords = 12;
constexpr size_t kBip39SeedBytes = 64;
constexpr size_t kSignatureBytes = 64;
constexpr size_t kMaxMnemonicChars = 24 * 8 + 23;  // BIP39_MAX_MNEMONIC_LEN

// Solana account path used by Phantom, Solflare and the Solana CLI.
constexpr const char* kSolanaPath = "m/44'/501'/0'/0'";
constexpr uint32_t kHardened = 0x80000000u;

// Must be called once before any other crypto function.
bool init();

// 12-word English BIP-39 mnemonic held in a wiped buffer.
class Mnemonic {
 public:
  bool fromEntropy(const uint8_t* entropy, size_t len);
  // Accepts user input: lower-cases and collapses whitespace, then checks the
  // words and checksum. Returns false for anything that is not a valid mnemonic.
  bool fromText(const std::string& text);

  const char* c_str() const { return reinterpret_cast<const char*>(text_.data()); }
  size_t wordCount() const { return wordCount_; }
  // Copies word `index` (0-based) into out (at least 9 bytes).
  bool word(size_t index, char* out, size_t outLen) const;
  bool equals(const Mnemonic& other) const;  // constant time over the buffer
  void wipe();

 private:
  Secret<kMaxMnemonicChars + 1> text_;
  size_t wordCount_ = 0;
};

// BIP-39 seed (PBKDF2-HMAC-SHA512, 2048 rounds).
void mnemonicToSeed(const Mnemonic& m, const char* passphrase, Secret<kBip39SeedBytes>& seed);

// SLIP-0010 ed25519 node. Only hardened derivation exists for ed25519.
struct Slip10Node {
  Secret<32> key;
  Secret<32> chainCode;
};

void slip10Master(const uint8_t* seed, size_t seedLen, Slip10Node& out);
// `index` must include the hardened bit; returns false otherwise.
bool slip10Child(const Slip10Node& parent, uint32_t index, Slip10Node& out);
// Parses "m/44'/501'/0'/0'"; every component must be hardened.
bool parsePath(const std::string& path, std::vector<uint32_t>& out);
bool slip10Derive(const uint8_t* seed, size_t seedLen, const std::vector<uint32_t>& path,
                  Slip10Node& out);

// Ed25519 key pair in libsodium's layout (64-byte secret = seed || pubkey).
struct KeyPair {
  PubKey publicKey{};
  Secret<crypto_sign_SECRETKEYBYTES> secretKey;
};

void keyPairFromSeed(const uint8_t seed32[32], KeyPair& out);
// Full pipeline: mnemonic -> BIP-39 seed -> SLIP-10 at kSolanaPath -> key pair.
void deriveSolanaKeyPair(const Mnemonic& m, KeyPair& out);

void signDetached(const KeyPair& kp, const uint8_t* msg, size_t len, uint8_t sig[kSignatureBytes]);
bool verifyDetached(const PubKey& pub, const uint8_t* msg, size_t len, const uint8_t sig[kSignatureBytes]);

}  // namespace wfw::crypto
