// Protocol basics, setup and backup flows.
#include "controller_harness.h"
#include "test.h"

using namespace test;
using wfw::ui::colors::kSetup;

TEST(ctrl_ping_and_protocol_errors) {
  Harness h;
  h.boot();
  json r = h.send({{"id", "a1"}, {"cmd", "ping"}});
  CHECK_EQ(r["ok"], true);
  CHECK_EQ(r["id"], "a1");
  const json p = json::parse(r["payload"].get<std::string>());
  CHECK_EQ(p["challenge"], "test-challenge-0001");
  CHECK_EQ(p["has_seed"], false);
  CHECK(p["wallets"].empty());
  CHECK_EQ(p["state"], "uninitialized");
  CHECK_EQ(p["protocol"], 2);
  CHECK_EQ(p["firmware"].get<std::string>(), std::string(WFW_VERSION));

  r = h.sendRaw("nonsense");
  CHECK_EQ(r["error"], "invalid_json");
  CHECK(r["id"].is_null());
  CHECK_EQ(h.send({{"cmd", 5}})["error"], "invalid_request");
  CHECK_EQ(h.send({{"id", {1}}, {"cmd", "ping"}})["error"], "invalid_request");
  r = h.send({{"id", 7}, {"cmd", "format_disk"}});
  CHECK_EQ(r["error"], "unknown_cmd");
  CHECK_EQ(r["id"], 7);
  CHECK_EQ(h.send({{"cmd", "get_pubkey"}})["error"], "not_initialized");
  CHECK_EQ(h.send({{"cmd", "sign_tx"}, {"message", "AA=="}})["error"], "not_initialized");
  CHECK_EQ(h.send({{"cmd", "show_seed"}})["error"], "not_initialized");
  h.ctrl->onOverflow();
  CHECK_EQ(h.out.back()["error"], "line_too_long");
  CHECK_EQ(h.top(), std::string("AURORA"));
}

TEST(ctrl_setup_shows_words_and_creates_wallet) {
  Harness h;
  h.boot();
  CHECK(h.send({{"id", 1}, {"cmd", "setup"}}).is_null());  // waits for the user
  CHECK_EQ(h.top(), std::string("WRITE DOWN THE"));
  CHECK(h.colorIs(kSetup));
  CHECK(h.buzzerHw.onCount >= 1);
  CHECK_EQ(h.send({{"cmd", "setup"}})["error"], "busy");
  CHECK_EQ(h.pingPayload()["busy"], true);

  h.click(Button::Middle);
  CHECK_EQ(h.top(), std::string("01/12 abandon"));
  CHECK_EQ(h.bottom(), std::string("M:NEXT"));
  h.click(Button::Middle);
  CHECK_EQ(h.top(), std::string("02/12 abandon"));
  h.click(Button::Left);
  CHECK_EQ(h.top(), std::string("01/12 abandon"));
  CHECK(h.hold(Button::Right).is_null());  // not on word 12 yet
  h.clicks(Button::Middle, 10);
  CHECK_EQ(h.top(), std::string("11/12 abandon"));
  CHECK(h.click(Button::Middle).is_null());
  CHECK_EQ(h.top(), std::string("12/12 about"));
  CHECK_EQ(h.bottom(), std::string("L:BACK HOLD R:OK"));
  CHECK(h.click(Button::Right).is_null());  // a short press is not enough
  CHECK(h.keystore->status() == wfw::storage::Keystore::Status::Empty);

  CHECK(h.hold(Button::Right).is_null());  // now the on-device backup check
  CHECK(h.top().rfind("WORD #", 0) == 0);
  CHECK(h.keystore->status() == wfw::storage::Keystore::Status::Empty);
  std::vector<std::string> words(11, "abandon");
  words.push_back("about");
  h.passQuiz(words);
  json r = h.out.back();
  CHECK_EQ(r["id"], 1);
  CHECK_EQ(r["ok"], true);
  CHECK_EQ(r["pubkey"], kAbandonAddress);
  CHECK_EQ(r["backup_confirmed"], true);
  CHECK_EQ(h.top(), std::string("WALLET CREATED"));

  const std::string name = "Aurora " + h.identity.deviceId().substr(0, 4);
  r = h.send({{"cmd", "get_pubkey"}});
  CHECK_EQ(r["pubkey"], kAbandonAddress);
  CHECK_EQ(r["name"], name);
  CHECK_EQ(h.send({{"cmd", "setup"}})["error"], "already_initialized");
  h.run(4000);
  CHECK_EQ(h.top(), name);
  CHECK_EQ(h.bottom(), std::string("HAgk..Kpqk"));
}

TEST(ctrl_setup_cancel_and_timeout) {
  Harness h;
  h.cfg.setupTimeoutS = 30;
  h.boot();
  h.send({{"id", 1}, {"cmd", "setup"}});
  h.click(Button::Middle);
  json r = h.send({{"id", 2}, {"cmd", "cancel"}});
  CHECK_EQ(r["cancelled"], true);
  REQUIRE(h.out.size() >= 2);
  CHECK_EQ(h.out[h.out.size() - 2]["error"], "cancelled");
  CHECK_EQ(h.out[h.out.size() - 2]["id"], 1);
  CHECK(!h.keystore->initialized());
  CHECK_EQ(h.send({{"cmd", "cancel"}})["cancelled"], false);

  h.send({{"id", 3}, {"cmd", "setup"}});
  r = h.run(31000);
  CHECK_EQ(r["error"], "timeout");
  CHECK_EQ(r["id"], 3);
  CHECK(!h.ctrl->sessionActive());
}

TEST(ctrl_backup_is_checked_on_the_device) {
  Harness h;
  h.boot();
  h.send({{"id", 1}, {"cmd", "setup"}});
  h.click(Button::Middle);
  std::vector<std::string> words;
  for (int i = 0; i < 12; ++i) {
    words.push_back(h.top().substr(6));
    if (i < 11) h.click(Button::Middle);
  }
  h.hold(Button::Right);
  // A wrong word sends the user back to the words; nothing is saved.
  REQUIRE(h.answerQuestion(words, /*correctly=*/false));
  CHECK_EQ(h.top(), std::string("WRITE DOWN THE"));
  CHECK_EQ(h.bottom(), std::string("WRONG WORD! M:GO"));
  CHECK(!h.keystore->initialized());
  CHECK(h.out.empty());
  CHECK_EQ(h.readWordsAndConfirm(), std::string(kAbandonMnemonic));
  CHECK_EQ(h.out.back()["ok"], true);
  CHECK(h.keystore->backupConfirmed());
  // Never shown again once confirmed; typed-in phrases are not accepted.
  CHECK_EQ(h.send({{"cmd", "show_seed"}})["error"], "backup_already_confirmed");
  CHECK_EQ(h.send({{"cmd", "confirm_backup"}, {"words", kAbandonMnemonic}})["error"],
           "unknown_cmd");
}

TEST(ctrl_show_seed_finishes_an_unchecked_backup) {
  Harness h;
  h.preinit();  // a wallet whose backup was never checked
  h.boot();
  CHECK(!h.keystore->backupConfirmed());
  CHECK(h.send({{"id", 9}, {"cmd", "show_seed"}}).is_null());
  CHECK_EQ(h.readWordsAndConfirm(), std::string(kAbandonMnemonic));
  CHECK_EQ(h.out.back()["id"], 9);
  CHECK_EQ(h.out.back()["backup_confirmed"], true);
  CHECK(h.keystore->backupConfirmed());
}

TEST(ctrl_hold_must_start_on_last_word) {
  Harness h;
  h.boot();
  h.send({{"cmd", "setup"}});
  h.clicks(Button::Middle, 11);
  CHECK_EQ(h.top(), std::string("11/12 abandon"));
  // Press R on word 11, then move to 12 while still holding: must not finish.
  h.raw[2] = true;
  h.run(500);
  h.click(Button::Middle);
  CHECK_EQ(h.top(), std::string("12/12 about"));
  CHECK(h.run(3000).is_null());
  h.raw[2] = false;
  h.run(100);
  CHECK(!h.keystore->initialized());
  h.hold(Button::Right);
  std::vector<std::string> words(11, "abandon");
  words.push_back("about");
  h.passQuiz(words);
  CHECK(h.keystore->initialized());
}

TEST(ctrl_display_failure_blocks_prompts) {
  Harness h;
  h.preinit();
  h.display.broken = true;
  h.boot();
  CHECK_EQ(h.pingPayload()["display_ok"], false);
  CHECK_EQ(h.send({{"cmd", "sign_tx"}, {"message", b64msg(kTransferMsgHex)}})["error"],
           "display_unavailable");
  h.display.broken = false;
  h.run(1100);  // renderer retries once a second
  CHECK_EQ(h.pingPayload()["display_ok"], true);
  CHECK(h.send({{"cmd", "sign_tx"}, {"message", b64msg(kTransferMsgHex)}}).is_null());
}
