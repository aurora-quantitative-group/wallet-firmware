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
  CHECK_EQ(r["pong"], true);
  CHECK_EQ(r["id"], "a1");
  CHECK_EQ(r["state"], "uninitialized");
  CHECK_EQ(r["protocol"], 1);
  CHECK_EQ(r["version"].get<std::string>(), std::string(WFW_VERSION));

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
  CHECK_EQ(h.send({{"cmd", "show_words"}})["error"], "not_initialized");
  h.ctrl->onOverflow();
  CHECK_EQ(h.out.back()["error"], "line_too_long");
  CHECK_EQ(h.top(), std::string("SOLPI WALLET"));
}

TEST(ctrl_setup_shows_words_and_creates_wallet) {
  Harness h;
  h.boot();
  CHECK(h.send({{"id", 1}, {"cmd", "setup"}}).is_null());  // waits for the user
  CHECK_EQ(h.top(), std::string("WRITE DOWN THE"));
  CHECK(h.colorIs(kSetup));
  CHECK(h.buzzerHw.onCount >= 1);
  CHECK_EQ(h.send({{"cmd", "setup"}})["error"], "busy");
  CHECK_EQ(h.send({{"cmd", "ping"}})["busy"], true);

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

  json r = h.hold(Button::Right);
  CHECK_EQ(r["id"], 1);
  CHECK_EQ(r["ok"], true);
  CHECK_EQ(r["pubkey"], kAbandonAddress);
  CHECK_EQ(r["backup_confirmed"], false);
  CHECK_EQ(h.top(), std::string("WALLET CREATED"));

  r = h.send({{"cmd", "get_pubkey"}});
  CHECK_EQ(r["pubkey"], kAbandonAddress);
  CHECK_EQ(r["name"], "SolPi-HAgk");
  CHECK_EQ(h.send({{"cmd", "setup"}})["error"], "already_initialized");
  h.run(4000);
  CHECK_EQ(h.top(), std::string("SolPi-HAgk"));
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

TEST(ctrl_backup_reshow_and_confirm) {
  Harness h;
  h.boot();
  h.send({{"cmd", "setup"}});
  const std::string words = h.readWordsAndConfirm();
  CHECK_EQ(words, std::string(kAbandonMnemonic));
  REQUIRE(h.keystore->initialized());

  // App's "back" button: show the words again.
  CHECK(h.send({{"id", 9}, {"cmd", "show_words"}}).is_null());
  CHECK_EQ(h.readWordsAndConfirm(), words);
  CHECK_EQ(h.out.back()["id"], 9);
  CHECK_EQ(h.out.back()["ok"], true);

  CHECK_EQ(h.send({{"cmd", "confirm_backup"}})["error"], "invalid_request");
  json r = h.send({{"cmd", "confirm_backup"},
                   {"words", "abandon abandon abandon abandon abandon abandon abandon abandon "
                             "abandon abandon abandon abandon"}});
  CHECK_EQ(r["error"], "phrase_mismatch");
  r = h.send({{"cmd", "confirm_backup"},
              {"words", "zoo zoo zoo zoo zoo zoo zoo zoo zoo zoo zoo wrong"}});
  CHECK_EQ(r["error"], "phrase_mismatch");
  CHECK(!h.keystore->backupConfirmed());

  r = h.send({{"cmd", "confirm_backup"}, {"words", "  ABANDON " + words.substr(8) + "\n"}});
  CHECK_EQ(r["ok"], true);
  CHECK(h.keystore->backupConfirmed());
  CHECK_EQ(h.send({{"cmd", "get_pubkey"}})["backup_confirmed"], true);
  CHECK_EQ(h.send({{"cmd", "show_words"}})["error"], "backup_already_confirmed");
  CHECK_EQ(h.send({{"cmd", "confirm_backup"}, {"words", words}})["error"],
           "backup_already_confirmed");
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
  CHECK(!h.hold(Button::Right).is_null());
  CHECK(h.keystore->initialized());
}

TEST(ctrl_display_failure_blocks_prompts) {
  Harness h;
  h.preinit();
  h.display.broken = true;
  h.boot();
  CHECK_EQ(h.send({{"cmd", "ping"}})["display_ok"], false);
  CHECK_EQ(h.send({{"cmd", "sign_tx"}, {"message", b64msg(kTransferMsgHex)}})["error"],
           "display_unavailable");
  h.display.broken = false;
  h.run(1100);  // renderer retries once a second
  CHECK_EQ(h.send({{"cmd", "ping"}})["display_ok"], true);
  CHECK(h.send({{"cmd", "sign_tx"}, {"message", b64msg(kTransferMsgHex)}}).is_null());
}
