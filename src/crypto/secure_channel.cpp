#include "crypto/secure_channel.h"

#include <sodium.h>

#include <cstring>

#include "crypto/hkdf.h"

namespace wfw::crypto {

static_assert(sizeof(SecureChannel::kTranscriptLabel) - 1 == 16, "label must be 16 bytes");

namespace {

void nonceFor(uint64_t counter, uint8_t nonce[SecureChannel::kNonceBytes]) {
  std::memset(nonce, 0, SecureChannel::kNonceBytes);
  for (int i = 0; i < 8; ++i) nonce[4 + i] = static_cast<uint8_t>(counter >> (8 * i));
}

uint64_t counterOf(const uint8_t nonce[SecureChannel::kNonceBytes], bool& wellFormed) {
  wellFormed = nonce[0] == 0 && nonce[1] == 0 && nonce[2] == 0 && nonce[3] == 0;
  uint64_t c = 0;
  for (int i = 0; i < 8; ++i) c |= static_cast<uint64_t>(nonce[4 + i]) << (8 * i);
  return c;
}

}  // namespace

void SecureChannel::transcript(const std::array<uint8_t, 32>& clientEph,
                               const std::array<uint8_t, 32>& deviceEph, const PubKey& devicePub,
                               uint8_t out[16 + 96]) {
  std::memcpy(out, kTranscriptLabel, 16);
  std::memcpy(out + 16, clientEph.data(), 32);
  std::memcpy(out + 48, deviceEph.data(), 32);
  std::memcpy(out + 80, devicePub.data(), 32);
}

bool SecureChannel::deriveKeys(const uint8_t shared[32], const std::array<uint8_t, 32>& clientEph,
                               const std::array<uint8_t, 32>& deviceEph, const PubKey& devicePub) {
  uint8_t t[16 + 96];
  transcript(clientEph, deviceEph, devicePub, t);
  uint8_t salt[crypto_hash_sha256_BYTES];
  crypto_hash_sha256(salt, t, sizeof(t));
  Secret<64> okm;
  if (!hkdfSha256(salt, sizeof(salt), shared, 32, reinterpret_cast<const uint8_t*>(kKeyInfo),
                  sizeof(kKeyInfo) - 1, okm.data(), okm.size())) {
    return false;
  }
  // okm = key(client->device) || key(device->client)
  if (role_ == Role::Device) {
    recvKey_.copyFrom(okm.data());
    sendKey_.copyFrom(okm.data() + 32);
  } else {
    sendKey_.copyFrom(okm.data());
    recvKey_.copyFrom(okm.data() + 32);
  }
  sendCounter_ = 0;
  recvCounter_ = 0;
  active_ = true;
  return true;
}

bool SecureChannel::accept(const std::array<uint8_t, 32>& clientEphemeral, const KeyPair& identity,
                           std::array<uint8_t, 32>& deviceEphemeral,
                           std::array<uint8_t, 64>& signature, std::string& err) {
  // The current channel (if any) stays usable until the new one is complete,
  // so a bogus handshake cannot knock out the app's session.
  Secret<32> devSecret;
  randombytes_buf(devSecret.data(), devSecret.size());
  crypto_scalarmult_base(deviceEphemeral.data(), devSecret.data());

  Secret<32> shared;
  // Fails for low-order points (all-zero shared secret).
  if (crypto_scalarmult(shared.data(), devSecret.data(), clientEphemeral.data()) != 0) {
    err = "invalid client key";
    return false;
  }
  uint8_t t[16 + 96];
  transcript(clientEphemeral, deviceEphemeral, identity.publicKey, t);
  crypto_sign_detached(signature.data(), nullptr, t, sizeof(t), identity.secretKey.data());
  if (!deriveKeys(shared.data(), clientEphemeral, deviceEphemeral, identity.publicKey)) {
    err = "key derivation failed";
    return false;
  }
  return true;
}

void SecureChannel::clientBegin(std::array<uint8_t, 32>& clientEphemeral) {
  reset();
  randombytes_buf(clientSecret_.data(), clientSecret_.size());
  crypto_scalarmult_base(clientPublic_.data(), clientSecret_.data());
  clientEphemeral = clientPublic_;
}

bool SecureChannel::clientComplete(const std::array<uint8_t, 32>& deviceEphemeral,
                                   const PubKey& deviceIdentity,
                                   const std::array<uint8_t, 64>& signature, std::string& err) {
  uint8_t t[16 + 96];
  transcript(clientPublic_, deviceEphemeral, deviceIdentity, t);
  if (crypto_sign_verify_detached(signature.data(), t, sizeof(t), deviceIdentity.data()) != 0) {
    err = "bad handshake signature";
    return false;
  }
  Secret<32> shared;
  if (crypto_scalarmult(shared.data(), clientSecret_.data(), deviceEphemeral.data()) != 0) {
    err = "invalid device key";
    return false;
  }
  clientSecret_.wipe();
  return deriveKeys(shared.data(), clientPublic_, deviceEphemeral, deviceIdentity);
}

void SecureChannel::reset() {
  active_ = false;
  sendKey_.wipe();
  recvKey_.wipe();
  sendCounter_ = 0;
  recvCounter_ = 0;
}

bool SecureChannel::open(const std::string& frame, std::string& plaintext, std::string& err) {
  if (!active_) {
    err = "no secure channel";
    return false;
  }
  auto raw = base64Decode(frame);
  if (!raw || raw->size() < kNonceBytes + crypto_aead_chacha20poly1305_ietf_ABYTES ||
      raw->size() > kNonceBytes + kMaxPlaintext + crypto_aead_chacha20poly1305_ietf_ABYTES) {
    err = "malformed frame";
    return false;
  }
  bool wellFormed = false;
  const uint64_t counter = counterOf(raw->data(), wellFormed);
  if (!wellFormed || counter != recvCounter_) {
    err = "unexpected nonce";
    return false;
  }
  const char* ad = role_ == Role::Device ? kAdClientToDevice : kAdDeviceToClient;
  const size_t ctLen = raw->size() - kNonceBytes;
  std::string out(ctLen - crypto_aead_chacha20poly1305_ietf_ABYTES, '\0');
  unsigned long long outLen = 0;
  if (crypto_aead_chacha20poly1305_ietf_decrypt(
          reinterpret_cast<uint8_t*>(out.data()), &outLen, nullptr, raw->data() + kNonceBytes,
          ctLen, reinterpret_cast<const uint8_t*>(ad), std::strlen(ad), raw->data(),
          recvKey_.data()) != 0) {
    err = "decryption failed";
    return false;
  }
  out.resize(static_cast<size_t>(outLen));
  ++recvCounter_;
  plaintext = std::move(out);
  return true;
}

std::string SecureChannel::seal(const std::string& plaintext) {
  const char* ad = role_ == Role::Device ? kAdDeviceToClient : kAdClientToDevice;
  Bytes frame(kNonceBytes + plaintext.size() + crypto_aead_chacha20poly1305_ietf_ABYTES);
  nonceFor(sendCounter_++, frame.data());
  unsigned long long ctLen = 0;
  crypto_aead_chacha20poly1305_ietf_encrypt(
      frame.data() + kNonceBytes, &ctLen, reinterpret_cast<const uint8_t*>(plaintext.data()),
      plaintext.size(), reinterpret_cast<const uint8_t*>(ad), std::strlen(ad), nullptr,
      frame.data(), sendKey_.data());
  frame.resize(kNonceBytes + static_cast<size_t>(ctLen));
  return base64Encode(frame);
}

}  // namespace wfw::crypto
