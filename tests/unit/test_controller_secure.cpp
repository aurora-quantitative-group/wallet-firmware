// Step 12 hardening: PIN-encrypted keystore and the encrypted channel.
#include "controller_harness.h"
#include "crypto/secure_channel.h"
#include "test.h"

using namespace test;
using namespace wfw;

namespace {

json signReq(int id = 1) {
  return {{"id", id}, {"cmd", "sign_tx"}, {"message", b64msg(kTransferMsgHex)}, {"risk", "low"}};
}

// Runs the client half of the handshake against the harness.
bool handshake(Harness& h, crypto::SecureChannel& client) {
  std::array<uint8_t, 32> eph{};
  client.clientBegin(eph);
  const json r = h.send({{"id", "hs"}, {"cmd", "handshake"},
                         {"client_pubkey", base64Encode(eph.data(), eph.size())}});
  if (r["ok"] != true) return false;
  const Bytes dev = *base64Decode(r["device_ephemeral"].get<std::string>());
  const Bytes sig = *base64Decode(r["signature"].get<std::string>());
  std::array<uint8_t, 32> devEph{};
  std::array<uint8_t, 64> sigArr{};
  std::copy(dev.begin(), dev.end(), devEph.begin());
  std::copy(sig.begin(), sig.end(), sigArr.begin());
  std::string err;
  return client.clientComplete(devEph, *decodeAddress(r["device_id"].get<std::string>()), sigArr,
                               err);
}

json openReply(crypto::SecureChannel& client, const json& frame) {
  std::string plain;
  std::string err;
  if (!frame.contains("enc") || !client.open(frame["enc"].get<std::string>(), plain, err)) {
    return json();
  }
  return Harness::normalise(json::parse(plain));
}

}  // namespace

TEST(pin_set_at_setup_then_unlock_to_sign) {
  Harness h;
  h.cfg.pinEnabled = true;
  h.boot();
  h.send({{"id", 1}, {"cmd", "setup"}});
  h.readWordsAndConfirm();
  CHECK_EQ(h.top(), std::string("SET NEW PIN"));
  CHECK_EQ(h.bottom(), std::string("0___ -L +R M"));
  h.enterPin("1234");
  CHECK_EQ(h.top(), std::string("REPEAT NEW PIN"));
  h.enterPin("1234");
  REQUIRE(!h.out.empty());
  CHECK_EQ(h.out.back()["ok"], true);
  CHECK_EQ(h.out.back()["pin_protected"], true);

  h.boot();  // power cycle
  json r = h.send({{"cmd", "get_pubkey"}});
  CHECK_EQ(r["locked"], true);
  CHECK_EQ(r["pubkey"], kAbandonAddress);
  std::string err;
  h.recipients->addAll({*decodeAddress(kRecipient)}, err);

  CHECK(h.send(signReq()).is_null());
  CHECK_EQ(h.top(), std::string("ENTER PIN"));
  h.enterPin("0000");
  CHECK_EQ(h.bottom(), std::string("WRONG PIN"));
  CHECK_EQ(h.top(), std::string("ENTER PIN 4 LEFT"));
  h.enterPin("1234");
  CHECK_EQ(h.top(), std::string("SEND 0.50 SOL"));
  r = h.hold(Button::Right);
  CHECK_EQ(r["ok"], true);
  const Bytes sig = *base64Decode(r["signature"].get<std::string>());
  CHECK_EQ(hexEncode(sig.data(), sig.size()), std::string(kTransferSigHex));
}

TEST(pin_mismatch_restarts_entry) {
  Harness h;
  h.cfg.pinEnabled = true;
  h.boot();
  h.send({{"cmd", "setup"}});
  h.readWordsAndConfirm();
  h.enterPin("1234");
  h.enterPin("4321");
  CHECK_EQ(h.top(), std::string("SET NEW PIN"));
  CHECK_EQ(h.bottom(), std::string("PIN MISMATCH"));
  CHECK(!h.keystore->initialized());
  h.run(2000);
  h.enterPin("5555");
  h.enterPin("5555");
  CHECK(h.keystore->initialized());
  CHECK(h.keystore->pinProtected());
}

TEST(pin_lockout_after_max_attempts) {
  Harness h;
  h.preinit(0, "2468");
  h.cfg.pinMaxAttempts = 2;
  h.boot();
  h.send(signReq(5));
  h.enterPin("1111");
  h.enterPin("1111");
  REQUIRE(!h.out.empty());
  CHECK_EQ(h.out.back()["error"], "pin_locked");
  CHECK_EQ(h.out.back()["id"], 5);
  CHECK_EQ(h.top(), std::string("TOO MANY TRIES"));
  CHECK_EQ(h.send(signReq(6))["error"], "pin_locked");
}

TEST(channel_handshake_and_encrypted_requests) {
  Harness h;
  h.preinit();
  h.boot();
  CHECK_EQ(json::parse(h.send({{"cmd", "ping"}})["payload"].get<std::string>())["secure_channel"],
           false);
  CHECK_EQ(h.send({{"enc", "AAAA"}})["error"], "no_secure_channel");

  crypto::SecureChannel client(crypto::SecureChannel::Role::Client);
  REQUIRE(handshake(h, client));
  const std::string frame = client.seal(Harness::envelope({{"id", 2}, {"cmd", "ping"}}).dump());
  json r = openReply(client, h.sendRaw(json{{"enc", frame}}.dump()));
  CHECK_EQ(r["id"], 2);
  CHECK_EQ(json::parse(r["payload"].get<std::string>())["secure_channel"], true);

  // Replays and forgeries are refused (in plaintext: no key to answer with).
  r = h.sendRaw(json{{"enc", frame}}.dump());
  CHECK_EQ(r["error"], "decrypt_failed");
  CHECK_EQ(h.send({{"enc", 5}})["error"], "invalid_request");

  // The channel survives errors; the next in-order frame still works.
  r = openReply(client,
                h.sendRaw(json{{"enc", client.seal("{\"command\":\"get_pubkey\"}")}}.dump()));
  CHECK_EQ(r["pubkey"], kAbandonAddress);

  h.ctrl->onLinkReset();
  r = h.send({{"cmd", "ping"}});
  CHECK_EQ(json::parse(r["payload"].get<std::string>())["secure_channel"], false);
}

TEST(channel_required_blocks_plaintext_signing) {
  Harness h;
  h.preinit();
  h.cfg.requireSecureChannel = true;
  h.boot();
  std::string err;
  h.recipients->addAll({*decodeAddress(kRecipient)}, err);
  CHECK_EQ(h.send(signReq())["error"], "secure_channel_required");
  CHECK_EQ(h.send({{"cmd", "ping"}})["ok"], true);
  CHECK_EQ(h.send({{"cmd", "get_pubkey"}})["ok"], true);

  crypto::SecureChannel client(crypto::SecureChannel::Role::Client);
  REQUIRE(handshake(h, client));
  CHECK(h.sendRaw(json{{"enc", client.seal(Harness::envelope(signReq(7)).dump())}}.dump())
            .is_null());
  CHECK_EQ(h.top(), std::string("SEND 0.50 SOL"));
  const json r = openReply(client, h.hold(Button::Right));
  CHECK_EQ(r["ok"], true);
  CHECK_EQ(r["id"], 7);
  CHECK(r.contains("signature"));
}

TEST(channel_handshake_errors) {
  Harness fresh;
  fresh.boot();
  // Signed by the device identity: works before a wallet exists.
  crypto::SecureChannel early(crypto::SecureChannel::Role::Client);
  CHECK(handshake(fresh, early));
  Harness h;
  h.preinit();
  h.boot();
  CHECK_EQ(h.send({{"cmd", "handshake"}})["error"], "invalid_request");
  CHECK_EQ(h.send({{"cmd", "handshake"}, {"client_pubkey", base64Encode(Bytes(31, 9))}})["error"],
           "invalid_request");
  CHECK_EQ(h.send({{"cmd", "handshake"}, {"client_pubkey", base64Encode(Bytes(32, 0))}})["error"],
           "handshake_failed");
}
