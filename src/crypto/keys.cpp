#include "crypto/keys.h"

#include <sodium.h>

#include <cctype>
#include <cstring>

extern "C" {
#include "bip39.h"
}

namespace wfw::crypto {

static_assert(kMaxMnemonicChars == BIP39_MAX_MNEMONIC_LEN, "mnemonic buffer size mismatch");

bool init() { return sodium_init() >= 0; }

// ---------------------------------------------------------------- Mnemonic --

namespace {

size_t countWords(const char* s) {
  size_t n = 0;
  bool inWord = false;
  for (; *s; ++s) {
    if (*s == ' ') {
      inWord = false;
    } else if (!inWord) {
      inWord = true;
      ++n;
    }
  }
  return n;
}

}  // namespace

bool Mnemonic::fromEntropy(const uint8_t* entropy, size_t len) {
  wipe();
  const char* m = mnemonic_from_data(entropy, len);
  if (m == nullptr) return false;
  const size_t n = std::strlen(m);
  const bool fits = n <= kMaxMnemonicChars;
  if (fits) std::memcpy(text_.data(), m, n);
  mnemonic_clear();  // wipe trezor-crypto's static copy
  if (!fits) return false;
  wordCount_ = countWords(c_str());
  return true;
}

bool Mnemonic::fromText(const std::string& text) {
  wipe();
  size_t out = 0;
  bool pendingSpace = false;
  for (const char ch : text) {
    const auto c = static_cast<unsigned char>(ch);
    if (std::isspace(c)) {
      pendingSpace = out > 0;
      continue;
    }
    if (c >= 0x80) {  // BIP-39 English words are pure ASCII
      wipe();
      return false;
    }
    if (pendingSpace) {
      if (out >= kMaxMnemonicChars) {
        wipe();
        return false;
      }
      text_[out++] = ' ';
      pendingSpace = false;
    }
    if (out >= kMaxMnemonicChars) {
      wipe();
      return false;
    }
    text_[out++] = static_cast<uint8_t>(std::tolower(c));
  }
  if (out == 0 || mnemonic_check(c_str()) != 1) {
    wipe();
    return false;
  }
  wordCount_ = countWords(c_str());
  return true;
}

bool Mnemonic::word(size_t index, char* out, size_t outLen) const {
  const char* s = c_str();
  size_t current = 0;
  while (*s) {
    const char* start = s;
    while (*s && *s != ' ') ++s;
    if (current == index) {
      const size_t n = static_cast<size_t>(s - start);
      if (n + 1 > outLen) return false;
      std::memcpy(out, start, n);
      out[n] = '\0';
      return true;
    }
    ++current;
    if (*s == ' ') ++s;
  }
  return false;
}

bool Mnemonic::equals(const Mnemonic& other) const {
  return sodium_memcmp(text_.data(), other.text_.data(), text_.size()) == 0;
}

void Mnemonic::wipe() {
  text_.wipe();
  wordCount_ = 0;
}

void mnemonicToSeed(const Mnemonic& m, const char* passphrase, Secret<kBip39SeedBytes>& seed) {
  mnemonic_to_seed(m.c_str(), passphrase, seed.data(), nullptr);
}

// ---------------------------------------------------------------- SLIP-10 --

namespace {

void hmacSha512(const uint8_t* key, size_t keyLen, const uint8_t* a, size_t aLen,
                const uint8_t* b, size_t bLen, Secret<64>& out) {
  crypto_auth_hmacsha512_state st;
  crypto_auth_hmacsha512_init(&st, key, keyLen);
  crypto_auth_hmacsha512_update(&st, a, aLen);
  if (bLen) crypto_auth_hmacsha512_update(&st, b, bLen);
  crypto_auth_hmacsha512_final(&st, out.data());
  sodium_memzero(&st, sizeof(st));
}

void splitInto(const Secret<64>& i, Slip10Node& out) {
  out.key.copyFrom(i.data());
  out.chainCode.copyFrom(i.data() + 32);
}

}  // namespace

void slip10Master(const uint8_t* seed, size_t seedLen, Slip10Node& out) {
  static const char kCurveKey[] = "ed25519 seed";
  Secret<64> i;
  hmacSha512(reinterpret_cast<const uint8_t*>(kCurveKey), sizeof(kCurveKey) - 1, seed, seedLen,
             nullptr, 0, i);
  splitInto(i, out);
}

bool slip10Child(const Slip10Node& parent, uint32_t index, Slip10Node& out) {
  if ((index & kHardened) == 0) return false;
  // data = 0x00 || k_par || ser32(i)
  Secret<1 + 32 + 4> data;
  data[0] = 0;
  std::memcpy(data.data() + 1, parent.key.data(), 32);
  data[33] = static_cast<uint8_t>(index >> 24);
  data[34] = static_cast<uint8_t>(index >> 16);
  data[35] = static_cast<uint8_t>(index >> 8);
  data[36] = static_cast<uint8_t>(index);
  Secret<64> i;
  hmacSha512(parent.chainCode.data(), 32, data.data(), data.size(), nullptr, 0, i);
  splitInto(i, out);
  return true;
}

bool parsePath(const std::string& path, std::vector<uint32_t>& out) {
  out.clear();
  if (path.size() < 1 || path[0] != 'm') return false;
  size_t pos = 1;
  while (pos < path.size()) {
    if (path[pos] != '/') return false;
    ++pos;
    uint64_t value = 0;
    size_t digits = 0;
    while (pos < path.size() && std::isdigit(static_cast<unsigned char>(path[pos]))) {
      value = value * 10 + static_cast<uint64_t>(path[pos] - '0');
      if (value >= kHardened) return false;
      ++pos;
      ++digits;
    }
    if (digits == 0 || pos >= path.size() || (path[pos] != '\'' && path[pos] != 'h')) return false;
    ++pos;
    out.push_back(static_cast<uint32_t>(value) | kHardened);
  }
  return true;
}

bool slip10Derive(const uint8_t* seed, size_t seedLen, const std::vector<uint32_t>& path,
                  Slip10Node& out) {
  Slip10Node node;
  slip10Master(seed, seedLen, node);
  for (const uint32_t index : path) {
    Slip10Node child;
    if (!slip10Child(node, index, child)) return false;
    node.key.copyFrom(child.key);
    node.chainCode.copyFrom(child.chainCode);
  }
  out.key.copyFrom(node.key);
  out.chainCode.copyFrom(node.chainCode);
  return true;
}

// ------------------------------------------------------------------ Ed25519 --

void keyPairFromSeed(const uint8_t seed32[32], KeyPair& out) {
  crypto_sign_seed_keypair(out.publicKey.data(), out.secretKey.data(), seed32);
}

void deriveSolanaKeyPair(const Mnemonic& m, KeyPair& out) {
  Secret<kBip39SeedBytes> seed;
  mnemonicToSeed(m, "", seed);
  std::vector<uint32_t> path;
  parsePath(kSolanaPath, path);
  Slip10Node node;
  slip10Derive(seed.data(), seed.size(), path, node);
  keyPairFromSeed(node.key.data(), out);
}

void signDetached(const KeyPair& kp, const uint8_t* msg, size_t len, uint8_t sig[kSignatureBytes]) {
  crypto_sign_detached(sig, nullptr, msg, len, kp.secretKey.data());
}

bool verifyDetached(const PubKey& pub, const uint8_t* msg, size_t len,
                    const uint8_t sig[kSignatureBytes]) {
  return crypto_sign_verify_detached(sig, msg, len, pub.data()) == 0;
}

}  // namespace wfw::crypto
