#include <sodium.h>

#include "crypto/hkdf.h"
#include "crypto/secure_channel.h"
#include "test.h"

using namespace wfw;
using namespace wfw::crypto;

namespace {

void makeIdentity(KeyPair& kp) {
  uint8_t seed[32];
  randombytes_buf(seed, sizeof(seed));
  keyPairFromSeed(seed, kp);
}

struct Pair {
  SecureChannel device{SecureChannel::Role::Device};
  SecureChannel client{SecureChannel::Role::Client};
  KeyPair identity;
  Pair() { makeIdentity(identity); }

  bool connect() {
    std::array<uint8_t, 32> clientEph{};
    std::array<uint8_t, 32> deviceEph{};
    std::array<uint8_t, 64> sig{};
    std::string err;
    client.clientBegin(clientEph);
    return device.accept(clientEph, identity, deviceEph, sig, err) &&
           client.clientComplete(deviceEph, identity.publicKey, sig, err);
  }
};

}  // namespace

TEST(hkdf_rfc5869_case1) {
  const Bytes ikm(22, 0x0b);
  const Bytes salt = *hexDecode("000102030405060708090a0b0c");
  const Bytes info = *hexDecode("f0f1f2f3f4f5f6f7f8f9");
  uint8_t okm[42];
  REQUIRE(hkdfSha256(salt.data(), salt.size(), ikm.data(), ikm.size(), info.data(), info.size(),
                     okm, sizeof(okm)));
  CHECK_EQ(hexEncode(okm, sizeof(okm)),
           std::string("3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf34007208d5"
                       "b887185865"));
}

TEST(hkdf_rfc5869_case3_empty_salt_and_info) {
  const Bytes ikm(22, 0x0b);
  uint8_t okm[42];
  REQUIRE(hkdfSha256(nullptr, 0, ikm.data(), ikm.size(), nullptr, 0, okm, sizeof(okm)));
  CHECK_EQ(hexEncode(okm, sizeof(okm)),
           std::string("8da4e775a563c18f715f802a063c5a31b8a11f5c5ee1879ec3454e5f3c738d2d9d201395fa"
                       "a4b61a96c8"));
  uint8_t prk[32];
  hkdfSha256Extract(nullptr, 0, ikm.data(), ikm.size(), prk);
  uint8_t tooMuch[1];
  CHECK(!hkdfSha256Expand(prk, nullptr, 0, tooMuch, 255 * 32 + 1));
}

TEST(channel_round_trip_both_directions) {
  Pair p;
  REQUIRE(p.connect());
  CHECK(p.device.active() && p.client.active());
  for (int i = 0; i < 3; ++i) {
    std::string out;
    std::string err;
    const std::string req = "{\"cmd\":\"ping\",\"id\":" + std::to_string(i) + "}";
    REQUIRE(p.device.open(p.client.seal(req), out, err));
    CHECK_EQ(out, req);
    const std::string resp = "{\"ok\":true}";
    REQUIRE(p.client.open(p.device.seal(resp), out, err));
    CHECK_EQ(out, resp);
  }
}

TEST(channel_rejects_replay_reorder_and_tamper) {
  Pair p;
  REQUIRE(p.connect());
  std::string out;
  std::string err;
  const std::string first = p.client.seal("one");
  const std::string second = p.client.seal("two");
  CHECK(!p.device.open(second, out, err));  // out of order
  REQUIRE(p.device.open(first, out, err));
  CHECK(!p.device.open(first, out, err));  // replay
  Bytes raw = *base64Decode(second);
  raw.back() ^= 0x01;
  CHECK(!p.device.open(base64Encode(raw), out, err));  // tamper
  CHECK(p.device.open(second, out, err));
  CHECK_EQ(out, std::string("two"));
  // A frame sealed for the device cannot be opened by the client.
  CHECK(!p.client.open(p.client.seal("x"), out, err));
  CHECK(!p.device.open("not base64!", out, err));
}

TEST(channel_client_rejects_wrong_identity) {
  SecureChannel device(SecureChannel::Role::Device);
  SecureChannel client(SecureChannel::Role::Client);
  KeyPair real;
  makeIdentity(real);
  KeyPair impostor;
  makeIdentity(impostor);
  std::array<uint8_t, 32> clientEph{};
  std::array<uint8_t, 32> deviceEph{};
  std::array<uint8_t, 64> sig{};
  std::string err;
  client.clientBegin(clientEph);
  REQUIRE(device.accept(clientEph, impostor, deviceEph, sig, err));
  CHECK(!client.clientComplete(deviceEph, real.publicKey, sig, err));
  CHECK(!client.active());
}

TEST(channel_rejects_low_order_client_key) {
  SecureChannel device(SecureChannel::Role::Device);
  KeyPair id;
  makeIdentity(id);
  std::array<uint8_t, 32> zero{};
  std::array<uint8_t, 32> deviceEph{};
  std::array<uint8_t, 64> sig{};
  std::string err;
  CHECK(!device.accept(zero, id, deviceEph, sig, err));
  CHECK(!device.active());
}
