// sign_tx: clear signing, device risk rule, approval and rejection.
#include "controller_harness.h"
#include "test.h"

using namespace test;
using namespace wfw;
using wfw::ui::colors::kHigh;
using wfw::ui::colors::kLow;
using wfw::ui::colors::kMedium;

namespace {

json signReq(const char* hexMsg, const char* risk = "low", int id = 1) {
  return {{"id", id}, {"cmd", "sign_tx"}, {"message", b64msg(hexMsg)}, {"risk", risk}};
}

void trustRecipient(Harness& h) {
  std::string err;
  h.recipients->addAll({*decodeAddress(kRecipient)}, err);
}

}  // namespace

TEST(sign_known_recipient_is_green_and_signature_matches_web3js) {
  Harness h;
  h.preinit();
  h.boot();
  trustRecipient(h);
  CHECK(h.send(signReq(kTransferMsgHex)).is_null());
  CHECK_EQ(h.top(), std::string("SEND 0.50 SOL"));
  CHECK_EQ(h.bottom(), std::string("TO 7xKX..gAsU"));
  CHECK(h.colorIs(kLow));
  CHECK(h.buzzerHw.onCount >= 1);

  const json r = h.hold(Button::Right);
  CHECK_EQ(r["ok"], true);
  CHECK_EQ(r["id"], 1);
  CHECK_EQ(r["risk"], "low");
  const Bytes sig = *base64Decode(r["signature"].get<std::string>());
  CHECK_EQ(hexEncode(sig.data(), sig.size()), std::string(kTransferSigHex));
  CHECK_EQ(h.top(), std::string("SIGNED"));
}

TEST(sign_new_recipient_is_yellow_then_remembered) {
  Harness h;
  h.preinit();
  h.boot();
  h.send(signReq(kTransferMsgHex, "low"));
  CHECK(h.colorIs(kMedium));
  json r = h.hold(Button::Right);
  CHECK_EQ(r["ok"], true);
  CHECK_EQ(r["risk"], "medium");
  CHECK_EQ(r["reason"], "NEW RECIPIENT");
  CHECK(h.recipients->contains(*decodeAddress(kRecipient)));

  h.run(2000);
  h.send(signReq(kTransferMsgHex, "low", 2));
  CHECK(h.colorIs(kLow));
  r = h.hold(Button::Right);
  CHECK_EQ(r["risk"], "low");
}

TEST(sign_compromised_app_is_red_and_rejected) {
  // Demo toggle: the app claims "low" but the message sends 2 SOL (over the
  // 1 SOL limit) to an address this device has never paid.
  Harness h;
  h.preinit();
  h.boot();
  h.send(signReq(kV0MsgHex, "low"));
  CHECK_EQ(h.top(), std::string("SEND 2.00 SOL"));
  CHECK_EQ(h.bottom(), std::string("TO 7xKX..gAsU"));
  CHECK(h.colorIs(kHigh));
  CHECK(h.buzzerHw.on);  // beep, then the long tone
  h.run(300);
  CHECK(h.buzzerHw.on);

  // Red needs the risk page to be seen before a hold can approve.
  CHECK(h.hold(Button::Right).is_null());
  CHECK_EQ(h.bottom(), std::string("M: SEE ALL PAGES"));
  const json r = h.click(Button::Left);
  CHECK_EQ(r["ok"], false);
  CHECK_EQ(r["error"], "rejected");
  CHECK_EQ(r["risk"], "high");
  CHECK_EQ(r["reason"], "NEW+OVER LIMIT");
  CHECK_EQ(h.top(), std::string("REJECTED"));
  CHECK(!h.recipients->contains(*decodeAddress(kRecipient)));
}

TEST(sign_middle_scrolls_full_address_then_red_can_be_approved) {
  Harness h;
  h.preinit();
  h.boot();
  h.send(signReq(kV0MsgHex, "low"));
  h.click(Button::Middle);
  CHECK_EQ(h.top(), std::string("7xKXtg2CW87d97TX"));
  CHECK_EQ(h.bottom(), std::string("JSDpbD5jBkheTqA8"));
  h.click(Button::Middle);
  CHECK_EQ(h.top(), std::string("3TZRuJosgAsU"));
  h.click(Button::Middle);
  CHECK_EQ(h.top(), std::string("RISK: HIGH"));
  CHECK_EQ(h.bottom(), std::string("NEW+OVER LIMIT"));
  h.click(Button::Middle);
  CHECK_EQ(h.top(), std::string("HOLD R TO SIGN"));
  h.click(Button::Middle);
  CHECK_EQ(h.top(), std::string("SEND 2.00 SOL"));  // wraps around
  const json r = h.hold(Button::Right);
  CHECK_EQ(r["ok"], true);
  CHECK_EQ(r["risk"], "high");
}

TEST(sign_unknown_program_is_red_and_blind_signing_can_be_disabled) {
  Harness h;
  h.preinit();
  h.cfg.allowBlindSigning = false;
  h.boot();
  h.send(signReq(kUnknownMsgHex, "low"));
  CHECK_EQ(h.top(), std::string("UNKNOWN TX"));
  CHECK_EQ(h.bottom(), std::string("UNKNOWN PROGRAM"));
  CHECK(h.colorIs(kHigh));
  h.clicks(Button::Middle, 3);  // see every page
  CHECK(h.hold(Button::Right).is_null());
  CHECK_EQ(h.bottom(), std::string("CANNOT SIGN"));
  CHECK_EQ(h.click(Button::Left)["error"], "rejected");
}

TEST(sign_memo_and_priority_fee_pages) {
  Harness h;
  h.preinit();
  h.boot();
  trustRecipient(h);
  h.send(signReq(kBudgetMemoMsgHex));
  CHECK_EQ(h.top(), std::string("SEND"));
  CHECK_EQ(h.bottom(), std::string("0.000000042 SOL"));
  CHECK(h.hold(Button::Right).is_null());  // memo and fee pages not seen yet
  std::vector<std::string> tops;
  for (int i = 0; i < 7; ++i) {
    h.click(Button::Middle);
    tops.push_back(h.top() + "|" + h.bottom());
  }
  CHECK(std::find(tops.begin(), tops.end(), "MEMO:|coffee for alice") != tops.end());
  CHECK(std::find(tops.begin(), tops.end(), "PRIORITY FEE|<=0.000015 SOL") != tops.end());
  CHECK_EQ(h.hold(Button::Right)["ok"], true);
}

TEST(sign_hold_that_began_before_the_prompt_never_approves) {
  Harness h;
  h.preinit();
  h.boot();
  trustRecipient(h);
  h.raw[2] = true;
  h.run(500);
  h.send(signReq(kTransferMsgHex));
  CHECK(h.run(4000).is_null());
  h.raw[2] = false;
  h.run(100);
  CHECK(h.ctrl->sessionActive());
  CHECK_EQ(h.hold(Button::Right)["ok"], true);
}

TEST(sign_validation_errors) {
  Harness h;
  h.preinit();
  h.boot();
  CHECK_EQ(h.send({{"cmd", "sign_tx"}})["error"], "invalid_request");
  CHECK_EQ(h.send({{"cmd", "sign_tx"}, {"message", "%%%"}})["error"], "invalid_request");
  CHECK_EQ(h.send({{"cmd", "sign_tx"}, {"message", b64msg(kTransferMsgHex)}, {"risk", "LOW"}})["error"],
           "invalid_request");
  json r = h.send({{"cmd", "sign_tx"}, {"message", base64Encode(Bytes{1, 2, 3})}});
  CHECK_EQ(r["error"], "invalid_message");
  CHECK(!h.ctrl->sessionActive());

  // A different wallet is not a signer of the abandon wallet's transfer.
  Harness other;
  other.preinit(0x11);
  other.boot();
  CHECK_EQ(other.send(signReq(kTransferMsgHex))["error"], "not_signer");
}

TEST(sign_busy_cancel_timeout_and_disconnect) {
  Harness h;
  h.preinit();
  h.cfg.reviewTimeoutS = 10;
  h.boot();
  h.send(signReq(kTransferMsgHex, "medium", 1));
  CHECK_EQ(h.send(signReq(kTransferMsgHex, "low", 2))["error"], "busy");
  CHECK_EQ(h.send({{"cmd", "ping"}})["busy"], true);
  CHECK_EQ(h.send({{"cmd", "get_pubkey"}})["ok"], true);
  json r = h.run(10100);
  CHECK_EQ(r["error"], "timeout");
  CHECK_EQ(r["id"], 1);

  h.send(signReq(kTransferMsgHex, "low", 3));
  h.send({{"cmd", "cancel"}});
  CHECK_EQ(h.out[h.out.size() - 2]["error"], "cancelled");

  h.send(signReq(kTransferMsgHex, "low", 4));
  const size_t before = h.out.size();
  h.ctrl->onLinkReset();
  CHECK_EQ(h.out.size(), before);  // nobody to answer
  CHECK(!h.ctrl->sessionActive());
  CHECK_EQ(h.top(), std::string("DISCONNECTED"));
}

TEST(sign_missing_risk_defaults_to_medium) {
  Harness h;
  h.preinit();
  h.boot();
  trustRecipient(h);
  h.send({{"cmd", "sign_tx"}, {"message", b64msg(kTransferMsgHex)}, {"reason", "model timeout"}});
  CHECK(h.colorIs(kMedium));
  h.clicks(Button::Middle, 3);
  CHECK_EQ(h.top(), std::string("RISK: MEDIUM"));
  CHECK_EQ(h.bottom(), std::string("model timeout"));
}
