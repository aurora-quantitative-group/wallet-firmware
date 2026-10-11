// The device menu: wallet list with balances, the app kill switch, and
// erasing the wallet (from the menu and from the app's `reset`).
#include "controller_harness.h"
#include "test.h"

using namespace test;
using namespace wfw;

namespace {

// Opens the menu and moves to `item`, e.g. "> WALLETS".
void select(Harness& h, const std::string& item) {
  if (h.top().rfind("MENU", 0) != 0) h.click(Button::Middle);
  for (int i = 0; i < 6 && h.bottom() != "> " + item; ++i) h.click(Button::Middle);
  REQUIRE(h.bottom() == "> " + item);
  h.hold(Button::Right);
}

json balances() {
  return {{"cmd", "balances"},
          {"wallets",
           {{{"id", "sol-0"}, {"amount", "12.5"}, {"symbol", "SOL"}},
            {{"id", "sol-dev"}, {"amount", "1.25"}, {"symbol", "SOL"}},
            {{"id", "btc-0"}, {"amount", "0.00012345"}, {"symbol", "BTC"}}}}};
}

}  // namespace

TEST(menu_lists_items_and_returns_home) {
  Harness fresh;
  fresh.boot();
  fresh.click(Button::Middle);
  CHECK_EQ(fresh.top(), std::string("MENU 1/2  HOLD R"));
  CHECK_EQ(fresh.bottom(), std::string("> DISCONNECT APP"));
  fresh.click(Button::Left);
  CHECK_EQ(fresh.top(), std::string("AURORA"));

  Harness h;
  h.preinit();
  h.boot();
  h.click(Button::Middle);
  CHECK_EQ(h.bottom(), std::string("> WALLETS"));
  h.clicks(Button::Middle, 2);
  CHECK_EQ(h.bottom(), std::string("> ERASE WALLET"));
  h.run(61000);  // menus close on their own
  CHECK_EQ(h.top(), "Aurora " + h.identity.deviceId().substr(0, 4));
}

TEST(menu_wallet_list_shows_balances_and_addresses) {
  Harness h;
  h.preinit();
  h.boot();
  select(h, "WALLETS");
  CHECK_EQ(h.top(), std::string("SOL MAIN 1/3"));
  CHECK_EQ(h.bottom(), std::string("BAL: OPEN APP"));
  CHECK_EQ(h.send(balances())["shown"], 3);
  CHECK_EQ(h.bottom(), std::string("12.5 SOL"));
  h.click(Button::Middle);
  CHECK_EQ(h.bottom(), std::string("HAgk..Kpqk"));
  h.click(Button::Middle);
  CHECK_EQ(h.top(), std::string("SOL DEVNET 2/3"));
  CHECK_EQ(h.bottom(), std::string("1.25 SOL"));
  h.clicks(Button::Middle, 2);
  CHECK_EQ(h.top(), std::string("BTC MAIN 3/3"));
  CHECK_EQ(h.bottom(), std::string("0.00012345 BTC"));
  h.click(Button::Middle);
  CHECK_EQ(h.bottom(), std::string("bc1qcr..6fyu"));
  h.click(Button::Left);
  CHECK_EQ(h.bottom(), std::string("> WALLETS"));

  const json bad = {{"cmd", "balances"},
                    {"wallets", {{{"id", "sol-0"}, {"amount", "1e9"}, {"symbol", "SOL"}}}}};
  CHECK_EQ(h.send(bad)["error"], "invalid_request");
  const json lower = {{"cmd", "balances"},
                      {"wallets", {{{"id", "sol-0"}, {"amount", "1"}, {"symbol", "sol"}}}}};
  CHECK_EQ(h.send(lower)["error"], "invalid_request");
}

TEST(menu_disconnect_blocks_every_app_request_until_reconnected) {
  Harness h;
  h.preinit();
  h.boot();
  select(h, "DISCONNECT APP");
  CHECK_EQ(h.top(), std::string("APP DISCONNECTED"));
  json r = h.send({{"cmd", "ping"}});
  CHECK_EQ(r["error"], "app_disconnected");
  CHECK(r["message"].get<std::string>().find("CONNECT APP") != std::string::npos);
  CHECK_EQ(h.send({{"cmd", "sign_tx"}, {"message", b64msg(kTransferMsgHex)}})["error"],
           "app_disconnected");
  h.run(3100);
  select(h, "CONNECT APP");
  CHECK_EQ(h.send({{"cmd", "ping"}})["ok"], true);
}

TEST(menu_erase_needs_every_warning_and_the_hold) {
  Harness h;
  h.preinit();
  h.boot();
  std::string err;
  h.recipients->addAll({*decodeAddress(kRecipient)}, err);
  select(h, "ERASE WALLET");
  CHECK_EQ(h.top(), std::string("ERASE WALLET?"));
  CHECK(h.colorIs(ui::colors::kHigh));
  h.hold(Button::Right);
  CHECK_EQ(h.bottom(), std::string("M: SEE ALL PAGES"));
  CHECK(h.keystore->initialized());
  h.click(Button::Middle);
  CHECK_EQ(h.top(), std::string("MOVE FUNDS FIRST"));
  h.click(Button::Middle);
  CHECK_EQ(h.top(), std::string("HOLD R TO ERASE"));
  h.hold(Button::Right);
  CHECK_EQ(h.top(), std::string("WALLET ERASED"));
  CHECK(!h.keystore->initialized());
  CHECK_EQ(h.recipients->size(), size_t{0});
  CHECK(h.out.empty());  // nothing to answer: started on the device
  CHECK_EQ(json::parse(h.send({{"cmd", "ping"}})["payload"].get<std::string>())["has_seed"], false);
}

TEST(app_reset_is_confirmed_or_rejected_on_the_device) {
  Harness h;
  h.preinit();
  h.boot();
  CHECK(h.send({{"id", 1}, {"cmd", "reset"}}).is_null());
  CHECK_EQ(h.top(), std::string("ERASE WALLET?"));
  json r = h.click(Button::Left);
  CHECK_EQ(r["error"], "rejected");
  CHECK(h.keystore->initialized());

  h.run(2000);
  CHECK(h.send({{"id", 2}, {"cmd", "reset"}}).is_null());
  h.clicks(Button::Middle, 2);
  r = h.hold(Button::Right);
  CHECK_EQ(r["id"], 2);
  CHECK_EQ(r["erased"], true);
  CHECK(!h.keystore->initialized());
  CHECK_EQ(h.send({{"cmd", "reset"}})["error"], "not_initialized");
}
