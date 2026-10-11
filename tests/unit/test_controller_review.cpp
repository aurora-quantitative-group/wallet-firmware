// Regression tests for the independent review findings: display integrity,
// memo labelling, blind signing, PIN counter persistence, auto-lock and
// handshake robustness.
#include "controller_harness.h"
#include "crypto/secure_channel.h"
#include "test.h"

using namespace test;
using namespace wfw;

namespace {

json signReq(const std::string& b64, int id = 1) {
  return {{"id", id}, {"cmd", "sign_tx"}, {"message", b64}, {"risk", "low"}};
}

// Legacy message: one Memo v2 instruction signed by the abandon wallet.
std::string memoMessage(const std::string& memo) {
  Bytes m = {1, 0, 1, 2};
  const PubKey self = *decodeAddress(kAbandonAddress);
  const PubKey memoProgram = *decodeAddress("MemoSq4gqABAXKb96qnH8TysNcWxMyWCqXgDLGmfcHr");
  m.insert(m.end(), self.begin(), self.end());
  m.insert(m.end(), memoProgram.begin(), memoProgram.end());
  m.insert(m.end(), 32, 7);  // blockhash
  m.insert(m.end(), {1, 1, 0, static_cast<uint8_t>(memo.size())});
  m.insert(m.end(), memo.begin(), memo.end());
  return base64Encode(m);
}

std::vector<std::string> allTops(Harness& h, int pages) {
  std::vector<std::string> tops;
  for (int i = 0; i < pages; ++i) {
    tops.push_back(h.top());
    h.click(Button::Middle);
  }
  return tops;
}

}  // namespace

TEST(review_display_failure_unsees_pages_and_blocks_hold) {
  Harness h;
  h.preinit();
  h.boot();
  h.send(signReq(b64msg(kTransferMsgHex)));  // new recipient: 5 pages, 4 required
  h.clicks(Button::Middle, 3);
  CHECK_EQ(h.top(), std::string("RISK: MEDIUM"));
  h.display.broken = true;  // e.g. a brownout while reviewing
  h.click(Button::Middle);
  CHECK(h.hold(Button::Right).is_null());  // never while the LCD is failing
  h.display.broken = false;
  h.run(1100);  // renderer recovers
  CHECK(h.hold(Button::Right).is_null());  // pages must be read again
  h.clicks(Button::Middle, 5);
  CHECK_EQ(h.hold(Button::Right)["ok"], true);
}

TEST(review_memo_pages_are_always_labelled) {
  Harness h;
  h.preinit();
  h.boot();
  // Tries to fake a firmware verdict inside the memo.
  const std::string spoof = "................RISK: LOW       KNOWN RECIPIENT ";
  h.send(signReq(memoMessage(spoof)));
  const auto tops = allTops(h, 6);
  CHECK_EQ(tops[0], std::string("SIGN TX"));
  CHECK_EQ(tops[1], std::string("MEMO 1/3"));
  CHECK_EQ(tops[2], std::string("MEMO 2/3"));
  CHECK_EQ(tops[3], std::string("MEMO 3/3"));
  CHECK_EQ(tops[4], std::string("RISK: LOW"));
  h.click(Button::Left);

  h.run(2000);
  h.send(signReq(memoMessage(std::string(120, 'x')), 2));
  const auto longTops = allTops(h, 8);
  CHECK_EQ(longTops[6], std::string("MEMO 6/6 (CUT)"));
}

TEST(review_blind_signing_off_by_default_and_never_learns_recipients) {
  // Corrupt the SetComputeUnitLimit tag (2 -> 0): an unrecognised instruction
  // next to a real transfer.
  std::string hexMsg = kBudgetMemoMsgHex;
  const auto pos = hexMsg.find("000502e0930400");
  REQUIRE(pos != std::string::npos);
  hexMsg.replace(pos, 14, "000500e0930400");

  Harness off;
  off.preinit();
  off.boot();
  CHECK(!off.cfg.allowBlindSigning);
  off.send(signReq(b64msg(hexMsg.c_str())));
  CHECK_EQ(off.top(), std::string("UNKNOWN TX"));
  off.clicks(Button::Middle, 12);
  CHECK(off.hold(Button::Right).is_null());
  CHECK_EQ(off.bottom(), std::string("CANNOT SIGN"));

  Harness on;
  on.preinit();
  on.cfg.allowBlindSigning = true;
  on.boot();
  on.send(signReq(b64msg(hexMsg.c_str())));
  on.clicks(Button::Middle, 12);
  CHECK_EQ(on.hold(Button::Right)["ok"], true);
  CHECK(!on.recipients->contains(*decodeAddress(kRecipient)));
}

TEST(pin_failures_survive_restart) {
  Harness h;
  h.preinit(0, "2468");
  h.cfg.pinMaxAttempts = 3;
  h.boot();
  h.send(signReq(b64msg(kTransferMsgHex)));
  h.enterPin("1111");
  h.enterPin("1111");
  h.send({{"cmd", "cancel"}});
  CHECK_EQ(h.keystore->pinFailures(), 2);

  h.boot();  // power cycle must not reset the counter
  h.send(signReq(b64msg(kTransferMsgHex), 2));
  h.enterPin("1111");
  CHECK_EQ(h.out.back()["error"], "pin_locked");
  CHECK_EQ(h.send(signReq(b64msg(kTransferMsgHex), 3))["error"], "pin_locked");
}

TEST(pin_success_resets_counter_and_auto_lock_relocks) {
  Harness h;
  h.preinit(0, "2468");
  h.cfg.autoLockS = 10;
  h.boot();
  std::string err;
  h.recipients->addAll({*decodeAddress(kRecipient)}, err);
  h.send(signReq(b64msg(kTransferMsgHex)));
  h.enterPin("1111");
  h.enterPin("2468");
  CHECK_EQ(h.keystore->pinFailures(), 0);
  CHECK_EQ(h.hold(Button::Right)["ok"], true);
  CHECK(h.keystore->ready());
  h.run(9000);
  CHECK(h.keystore->ready());
  h.run(2000);
  CHECK(!h.keystore->ready());
  CHECK_EQ(h.send({{"cmd", "get_pubkey"}})["locked"], true);
}

TEST(failed_handshake_keeps_existing_channel) {
  Harness h;
  h.preinit();
  h.boot();
  crypto::SecureChannel client(crypto::SecureChannel::Role::Client);
  std::array<uint8_t, 32> eph{};
  client.clientBegin(eph);
  json r = h.send({{"cmd", "handshake"}, {"client_pubkey", base64Encode(eph.data(), 32)}});
  std::array<uint8_t, 32> dev{};
  std::array<uint8_t, 64> sig{};
  const Bytes d = *base64Decode(r["device_ephemeral"].get<std::string>());
  const Bytes s = *base64Decode(r["signature"].get<std::string>());
  std::copy(d.begin(), d.end(), dev.begin());
  std::copy(s.begin(), s.end(), sig.begin());
  std::string err;
  REQUIRE(client.clientComplete(dev, h.identity.publicKey(), sig, err));

  CHECK_EQ(h.send({{"cmd", "handshake"}, {"client_pubkey", base64Encode(Bytes(32, 0))}})["error"],
           "handshake_failed");
  r = h.sendRaw(json{{"enc", client.seal(Harness::envelope({{"id", 5}, {"cmd", "ping"}}).dump())}}
                    .dump());
  std::string plain;
  REQUIRE(r.contains("enc"));
  REQUIRE(client.open(r["enc"].get<std::string>(), plain, err));
  CHECK_EQ(json::parse(plain)["id"], 5);
}
