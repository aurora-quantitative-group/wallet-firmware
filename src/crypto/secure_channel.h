#pragma once

#include <array>
#include <cstdint>
#include <string>

#include "crypto/encoding.h"
#include "crypto/keys.h"
#include "crypto/secret.h"

namespace wfw::crypto {

// Optional encrypted channel (build manual Step 12):
//   X25519 ephemeral key exchange -> HKDF-SHA256 -> ChaCha20-Poly1305 (IETF)
//   on every frame, with the device's ed25519 identity key signing the
//   handshake transcript. Byte layouts are specified in
//   docs/firmware/protocol-contract.md; both roles live here so the tests can
//   run a full client/device exchange.
class SecureChannel {
 public:
  enum class Role { Device, Client };

  static constexpr char kTranscriptLabel[] = "WFW-HANDSHAKE-V1";  // 16 bytes
  static constexpr char kKeyInfo[] = "wfw1 session keys";
  static constexpr char kAdClientToDevice[] = "wfw1-c2d";
  static constexpr char kAdDeviceToClient[] = "wfw1-d2c";
  static constexpr size_t kNonceBytes = 12;
  static constexpr size_t kMaxPlaintext = 64 * 1024;

  explicit SecureChannel(Role role) : role_(role) {}

  // Device side: given the client's ephemeral X25519 public key, generate our
  // own ephemeral key, derive the session keys and sign the transcript.
  bool accept(const std::array<uint8_t, 32>& clientEphemeral, const KeyPair& identity,
              std::array<uint8_t, 32>& deviceEphemeral, std::array<uint8_t, 64>& signature,
              std::string& err);

  // Client side helpers (used by tests and as a reference for the app).
  void clientBegin(std::array<uint8_t, 32>& clientEphemeral);
  bool clientComplete(const std::array<uint8_t, 32>& deviceEphemeral, const PubKey& deviceIdentity,
                      const std::array<uint8_t, 64>& signature, std::string& err);

  bool active() const { return active_; }
  void reset();

  // Frame = base64(nonce(12) || ciphertext || tag(16)); nonce = 0x00000000 ||
  // u64-le counter. Each direction counts from 0 and must arrive in order.
  bool open(const std::string& frame, std::string& plaintext, std::string& err);
  std::string seal(const std::string& plaintext);

  static void transcript(const std::array<uint8_t, 32>& clientEph,
                         const std::array<uint8_t, 32>& deviceEph, const PubKey& devicePub,
                         uint8_t out[16 + 96]);

 private:
  bool deriveKeys(const uint8_t shared[32], const std::array<uint8_t, 32>& clientEph,
                  const std::array<uint8_t, 32>& deviceEph, const PubKey& devicePub);

  Role role_;
  bool active_ = false;
  Secret<32> sendKey_;
  Secret<32> recvKey_;
  uint64_t sendCounter_ = 0;
  uint64_t recvCounter_ = 0;
  Secret<32> clientSecret_;  // client role only, between begin and complete
  std::array<uint8_t, 32> clientPublic_{};
};

}  // namespace wfw::crypto
