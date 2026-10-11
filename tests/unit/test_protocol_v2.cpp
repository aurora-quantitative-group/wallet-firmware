// Aurora protocol v2: wire envelope, signed identity, wallet manifest,
// pairing and wallet selection. These pin the contract the desktop app
// (wallet-software, src-tauri/src/device.rs) depends on.
#include <sys/stat.h>

#include "controller_harness.h"
#include "crypto/bitcoin.h"
#include "test.h"
#include "util/fs.h"

using namespace test;
using namespace wfw;

namespace {

// Verifies a {payload, signature} result the way the app does.
json verifiedPayload(const json& result, const PubKey& identity) {
  const std::string payload = result.at("payload").get<std::string>();
  const Bytes sig = *base64Decode(result.at("signature").get<std::string>());
  REQUIRE(sig.size() == 64);
  REQUIRE(crypto::verifyDetached(identity, reinterpret_cast<const uint8_t*>(payload.data()),
                                 payload.size(), sig.data()));
  return json::parse(payload);
}

}  // namespace

TEST(v2_wire_envelope) {
  Harness h;
  h.boot();
  h.sendRaw(R"({"id":"x1","command":"get_pubkey"})");
  const json err = h.wire.back();
  CHECK_EQ(err["id"], "x1");
  CHECK_EQ(err["code"], "not_initialized");
  CHECK_EQ(err["error"], "Aurora has no wallet yet. Set it up first.");
  CHECK(!err.contains("ok") && !err.contains("result"));
  h.sendRaw(R"({"id":7,"command":"cancel","params":{}})");
  const json ok = h.wire.back();
  CHECK_EQ(ok["id"], 7);
  CHECK_EQ(ok["result"]["cancelled"], false);
  CHECK(!ok.contains("error") && !ok["result"].contains("ok"));
  h.sendRaw(R"({"id":8,"command":"cancel","params":[1]})");
  CHECK_EQ(h.wire.back()["code"], "invalid_request");
  h.sendRaw(R"({"id":9,"cmd":"ping"})");  // protocol v1 shape
  CHECK_EQ(h.wire.back()["code"], "invalid_request");
}

TEST(v2_ping_is_signed_by_the_device_identity) {
  Harness h;
  h.boot();
  json r = h.send({{"id", 1}, {"cmd", "ping"}, {"challenge", "a1b2c3d4e5f60718"}});
  const json p = verifiedPayload(r, h.identity.publicKey());
  CHECK_EQ(p["challenge"], "a1b2c3d4e5f60718");
  CHECK_EQ(p["device_id"], h.identity.deviceId());
  CHECK_EQ(p["name"], "Aurora " + h.identity.deviceId().substr(0, 4));
  CHECK_EQ(p["has_seed"], false);
  // The signature covers the payload bytes: any change breaks it.
  std::string tampered = r["payload"].get<std::string>();
  tampered[tampered.find("false")] = 'F';
  const Bytes sig = *base64Decode(r["signature"].get<std::string>());
  CHECK(!crypto::verifyDetached(h.identity.publicKey(),
                                reinterpret_cast<const uint8_t*>(tampered.data()), tampered.size(),
                                sig.data()));
  CHECK_EQ(h.send({{"cmd", "ping"}, {"challenge", "short"}})["error"], "invalid_request");
  CHECK_EQ(h.send({{"cmd", "ping"}, {"challenge", 12345678}})["error"], "invalid_request");
}

TEST(v2_wallet_manifest_and_bitcoin_address) {
  Harness h;
  h.preinit(0, "2468");  // PIN-locked: addresses must still be reported
  h.boot();
  const json p = verifiedPayload(h.send({{"cmd", "ping"}}), h.identity.publicKey());
  CHECK_EQ(p["has_seed"], true);
  CHECK_EQ(p["state"], "locked");
  REQUIRE(p["wallets"].size() == 3);
  const json& w = p["wallets"];
  CHECK_EQ(w[0]["id"], "sol-0");
  CHECK_EQ(w[0]["network"], "mainnet");
  CHECK_EQ(w[0]["address"], kAbandonAddress);
  CHECK_EQ(w[1]["id"], "sol-dev");
  CHECK_EQ(w[1]["network"], "devnet");
  CHECK_EQ(w[1]["address"], kAbandonAddress);
  CHECK_EQ(w[2]["id"], "btc-0");
  CHECK_EQ(w[2]["chain"], "BTC");
  CHECK_EQ(w[2]["address"], "bc1qcr8te4kr609gcawutmrza0j4xv80jy8z306fyu");
  CHECK_EQ(w[2]["account"], 0);
}

TEST(v2_bip84_vectors) {
  // BIP-84 test vectors for "abandon x11 about".
  crypto::Mnemonic m;
  REQUIRE(m.fromText(kAbandonMnemonic));
  std::string a0;
  std::string a1;
  REQUIRE(crypto::bip84Address(m, 0, a0));
  REQUIRE(crypto::bip84Address(m, 1, a1));
  CHECK_EQ(a0, std::string("bc1qcr8te4kr609gcawutmrza0j4xv80jy8z306fyu"));
  CHECK(a1 != a0 && a1.rfind("bc1q", 0) == 0);
}

TEST(v2_identity_persists_and_is_private) {
  TempDir dir;
  std::string err;
  storage::DeviceIdentity a(dir.path);
  REQUIRE(a.loadOrCreate(err));
  storage::DeviceIdentity b(dir.path);
  REQUIRE(b.loadOrCreate(err));
  CHECK_EQ(a.deviceId(), b.deviceId());
  struct stat st {};
  REQUIRE(::stat(fs::joinPath(dir.path, "identity.key").c_str(), &st) == 0);
  CHECK_EQ(st.st_mode & 0777, 0600u);
  fs::removeFile(fs::joinPath(dir.path, "identity.key"), err);
}

TEST(v2_pairing_is_approved_on_the_device) {
  Harness h;
  h.preinit();
  h.boot();
  const json req = {{"id", 3}, {"cmd", "pair"}, {"challenge", "pair-challenge-01"}, {"code", "482193"}};
  CHECK(h.send(req).is_null());
  CHECK_EQ(h.top(), std::string("PAIR CODE 482193"));
  CHECK(h.buzzerHw.onCount >= 1);
  const json r = h.hold(Button::Right);
  CHECK_EQ(r["id"], 3);
  const json p = verifiedPayload(r, h.identity.publicKey());
  CHECK_EQ(p["challenge"], "pair-challenge-01");
  CHECK_EQ(p["code"], "482193");
  CHECK_EQ(p["paired"], true);
  CHECK_EQ(p["device_id"], h.identity.deviceId());

  h.run(2500);
  h.send({{"id", 4}, {"cmd", "pair"}, {"challenge", "pair-challenge-02"}, {"code", "1234"}});
  CHECK_EQ(h.click(Button::Left)["error"], "rejected");
  CHECK_EQ(h.send({{"cmd", "pair"}, {"challenge", "pair-challenge-03"}, {"code", "12"}})["error"],
           "invalid_request");
  CHECK_EQ(h.send({{"cmd", "pair"}, {"challenge", "pair-challenge-03"}, {"code", "12a4"}})["error"],
           "invalid_request");
  Harness fresh;
  fresh.boot();
  CHECK_EQ(fresh.send({{"cmd", "pair"}, {"challenge", "pair-challenge-04"}, {"code", "1234"}})["error"],
           "not_initialized");
}

TEST(v2_sign_tx_checks_the_selected_wallet) {
  Harness h;
  h.preinit();
  h.boot();
  std::string err;
  h.recipients->addAll({*decodeAddress(kRecipient)}, err);
  const std::string msg = b64msg(kTransferMsgHex);
  CHECK_EQ(h.send({{"cmd", "sign_tx"}, {"address", kRecipient}, {"message", msg}})["error"],
           "wrong_wallet");
  CHECK(h.send({{"id", 2}, {"cmd", "sign_tx"}, {"address", kAbandonAddress}, {"message", msg}})
            .is_null());
  const json r = h.hold(Button::Right);
  CHECK_EQ(r["id"], 2);
  const Bytes sig = *base64Decode(r["signature"].get<std::string>());
  CHECK_EQ(hexEncode(sig.data(), sig.size()), std::string(kTransferSigHex));
}
