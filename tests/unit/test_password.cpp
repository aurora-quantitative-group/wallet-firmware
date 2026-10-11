// App password: chosen at setup, encrypts the wallet on Aurora, and unlocks
// it after a restart, idle auto-lock or the app's lock button.
#include "controller_harness.h"
#include "test.h"

using namespace test;
using namespace wfw;

namespace {

json signReq(int id = 1) {
  return {{"id", id}, {"cmd", "sign_tx"}, {"message", b64msg(kTransferMsgHex)}, {"risk", "low"}};
}

std::string state(Harness& h) { return h.pingPayload()["state"]; }

}  // namespace

TEST(password_set_at_setup_encrypts_and_locks_after_restart) {
  Harness h;
  h.boot();
  CHECK_EQ(h.send({{"cmd", "setup"}, {"password", "short"}})["error"], "invalid_request");
  CHECK(h.send({{"id", 1}, {"cmd", "setup"}, {"password", "correct horse"}}).is_null());
  h.readWordsAndConfirm();
  CHECK_EQ(h.out.back()["pin_protected"], true);
  CHECK(h.keystore->ready());  // unlocked right after setup

  h.boot();  // restart: the wallet is encrypted at rest
  CHECK_EQ(state(h), std::string("locked"));
  CHECK_EQ(h.pingPayload()["pin_protected"], true);
  CHECK_EQ(h.send(signReq())["error"], "locked");  // no button PIN fallback
  json r = h.send({{"cmd", "unlock"}, {"password", "wrong horse"}});
  CHECK_EQ(r["error"], "wrong_password");
  CHECK_EQ(r["attempts_left"], 4);
  r = h.send({{"cmd", "unlock"}, {"password", "correct horse"}});
  CHECK_EQ(r["state"], "ready");
  CHECK_EQ(h.keystore->pinFailures(), 0);
  std::string err;
  h.recipients->addAll({*decodeAddress(kRecipient)}, err);
  h.send(signReq(2));
  CHECK_EQ(h.hold(Button::Right)["ok"], true);
}

TEST(password_lock_command_and_attempt_limit) {
  Harness h;
  h.preinit(0, "correct horse");
  h.cfg.pinMaxAttempts = 2;
  h.boot();
  CHECK_EQ(h.send({{"cmd", "unlock"}, {"password", "correct horse"}})["state"], "ready");
  CHECK_EQ(h.send({{"cmd", "lock"}})["locked"], true);
  CHECK_EQ(state(h), std::string("locked"));
  CHECK_EQ(h.send({{"cmd", "unlock"}, {"password", ""}})["error"], "invalid_request");
  CHECK_EQ(h.send({{"cmd", "unlock"}, {"password", "nope"}})["error"], "wrong_password");
  CHECK_EQ(h.send({{"cmd", "unlock"}, {"password", "nope"}})["error"], "pin_locked");
  h.boot();  // the count survives a restart
  CHECK_EQ(h.send({{"cmd", "unlock"}, {"password", "correct horse"}})["error"], "pin_locked");
  // Erasing still works without the password; the 12 words restore funds.
  h.send({{"id", 9}, {"cmd", "reset"}});
  h.clicks(Button::Middle, 2);
  CHECK_EQ(h.hold(Button::Right)["erased"], true);
}

TEST(password_unlock_on_unprotected_wallet_is_a_no_op) {
  Harness h;
  h.preinit();
  h.boot();
  CHECK_EQ(h.send({{"cmd", "unlock"}, {"password", "anything"}})["state"], "ready");
  CHECK_EQ(h.send({{"cmd", "lock"}})["locked"], false);
  CHECK_EQ(state(h), std::string("ready"));
  Harness fresh;
  fresh.boot();
  CHECK_EQ(fresh.send({{"cmd", "unlock"}, {"password", "anything"}})["error"], "not_initialized");
}
