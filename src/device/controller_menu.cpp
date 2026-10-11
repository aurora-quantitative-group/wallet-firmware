// The device menu (middle button on the home screen): wallet list with
// balances, the app kill switch, erase, and about. Also the app's `reset`
// and `balances` commands, which end up on the same screens.
#include <algorithm>
#include <cctype>

#include "device/controller.h"
#include "device/protocol.h"
#include "solana/format.h"
#include "util/log.h"

namespace wfw::device {

namespace {

constexpr const char* kWallets = "WALLETS";
constexpr const char* kDisconnect = "DISCONNECT APP";
constexpr const char* kConnect = "CONNECT APP";
constexpr const char* kErase = "ERASE WALLET";
constexpr const char* kAbout = "ABOUT";

bool validAmount(const std::string& s) {
  return !s.empty() && s.size() <= 16 && std::count(s.begin(), s.end(), '.') <= 1 &&
         std::all_of(s.begin(), s.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)) || c == '.'; });
}

bool validSymbol(const std::string& s) {
  return !s.empty() && s.size() <= 6 &&
         std::all_of(s.begin(), s.end(), [](char c) { return std::isupper(static_cast<unsigned char>(c)); });
}

}  // namespace

// ------------------------------------------------------------------ menu --

std::vector<std::string> Controller::menuItems() const {
  std::vector<std::string> items;
  if (s_.keystore.initialized()) items.push_back(kWallets);
  items.push_back(appDisconnected_ ? kConnect : kDisconnect);
  if (s_.keystore.initialized()) items.push_back(kErase);
  items.push_back(kAbout);
  return items;
}

void Controller::openMenu() {
  menuIndex_ = 0;
  showMenu();
}

void Controller::showMenu() {
  mode_ = Mode::Menu;
  hint_.clear();
  menuActivity_ = s_.clock.nowMs();
  sessionStart_ = menuActivity_;  // a selecting hold must start on this item
  const auto items = menuItems();
  menuIndex_ %= items.size();
  base_ = {"MENU " + std::to_string(menuIndex_ + 1) + "/" + std::to_string(items.size()) +
               "  HOLD R",
           "> " + items[menuIndex_], ui::colors::kIdle};
}

void Controller::onMenuButton(const ui::ButtonEvent& ev) {
  using Kind = ui::ButtonEvent::Kind;
  const auto items = menuItems();
  if (ev.kind == Kind::Press && ev.button == ui::Button::Left) {
    showIdle();
  } else if (ev.kind == Kind::Press && ev.button == ui::Button::Middle) {
    menuIndex_ = (menuIndex_ + 1) % items.size();
    showMenu();
  } else if (ev.kind == Kind::Hold && ev.button == ui::Button::Right && holdStartedInSession(ev)) {
    const std::string& item = items[menuIndex_ % items.size()];
    if (item == kWallets) {
      browseIndex_ = 0;
      showWalletPage();
    } else if (item == kDisconnect || item == kConnect) {
      setAppDisconnected(item == kDisconnect);
    } else if (item == kErase) {
      startErase();
    } else {
      showNotice("AURORA FW " WFW_VERSION, "ID " + s_.identity.deviceId().substr(0, 13),
                 ui::colors::kIdle, 4000);
    }
  }
}

// ----------------------------------------------------------- wallet list --
// Two pages per wallet: chain/network with the balance, then the address.

void Controller::showWalletPage() {
  mode_ = Mode::Browse;
  menuActivity_ = s_.clock.nowMs();
  const json wallets = walletManifest();
  if (wallets.empty()) {
    showMenu();
    return;
  }
  const size_t pages = wallets.size() * 2;
  browseIndex_ %= pages;
  const json& w = wallets[browseIndex_ / 2];
  const std::string chain = w.value("chain", "");
  const std::string net = w.value("network", "") == "mainnet" ? "MAIN" : "DEVNET";
  const std::string top = chain + " " + net + " " + std::to_string(browseIndex_ / 2 + 1) + "/" +
                          std::to_string(wallets.size());
  std::string bottom;
  if (browseIndex_ % 2 == 0) {
    const auto b = balances_.find(w.value("id", ""));
    if (b == balances_.end()) {
      bottom = "BAL: OPEN APP";
    } else {
      bottom = b->second.amount + " " + b->second.symbol;
      if (bottom.size() > kLcdCols) bottom = b->second.amount;  // never cut digits off
    }
  } else {
    const std::string address = w.value("address", "");
    bottom = chain == "BTC" ? address.substr(0, 6) + ".." + address.substr(address.size() - 4)
                            : abbreviateAddress(address);
  }
  base_ = {top, bottom, ui::colors::kIdle};
}

void Controller::onBrowseButton(const ui::ButtonEvent& ev) {
  if (ev.kind != ui::ButtonEvent::Kind::Press) return;
  if (ev.button == ui::Button::Left) {
    showMenu();
  } else if (ev.button == ui::Button::Middle) {
    ++browseIndex_;
    showWalletPage();
  }
}

void Controller::handleBalances(const Ctx& ctx, const json& params) {
  const auto list = params.find("wallets");
  if (list == params.end() || !list->is_array() || list->size() > 20) {
    replyError(ctx, err::kInvalidRequest, "wallets must be a list of up to 20 balances");
    return;
  }
  std::map<std::string, Balance> next;
  for (const auto& item : *list) {
    if (!item.is_object() || !item.contains("id") || !item["id"].is_string() ||
        !item.contains("amount") || !item["amount"].is_string() || !item.contains("symbol") ||
        !item["symbol"].is_string() || item["id"].get_ref<const std::string&>().size() > 16 ||
        !validAmount(item["amount"]) || !validSymbol(item["symbol"])) {
      replyError(ctx, err::kInvalidRequest, "each balance needs id, amount (digits) and symbol");
      return;
    }
    next[item["id"]] = {item["amount"], item["symbol"]};
  }
  balances_ = std::move(next);
  if (mode_ == Mode::Browse) showWalletPage();
  reply(ctx, {{"ok", true}, {"shown", balances_.size()}});
}

// --------------------------------------------------------- app kill switch --

void Controller::setAppDisconnected(bool disconnected) {
  appDisconnected_ = disconnected;
  if (disconnected) {
    LOG_INFO("apps disconnected from the device menu");
    if (sessionActive()) finishPendingError(err::kCancelled, "disconnected on Aurora");
    clearSecrets();
    channel_.reset();
    balances_.clear();
    showNotice("APP DISCONNECTED", "MENU: CONNECT", ui::colors::kIdle, 3000);
  } else {
    LOG_INFO("apps reconnected from the device menu");
    showNotice("APP CONNECTED", "", ui::colors::kLow, 2000);
  }
}

// ----------------------------------------------------------------- erase --
// From the menu or the app's `reset`: red pages that must all be seen, then
// the 2-second hold. Only the 12 recovery words can bring the wallet back.

void Controller::handleReset(const Ctx& ctx) {
  // Not checkReadyForPrompt: a wallet locked out by wrong passwords must still
  // be erasable (the hold on the device confirms it; the words restore funds).
  if (sessionActive()) {
    replyError(ctx, err::kBusy, "a device prompt is in progress");
    return;
  }
  if (!s_.screen.healthy()) {
    replyError(ctx, err::kDisplayUnavailable, "the device display is not responding");
    return;
  }
  if (!s_.keystore.initialized() && s_.keystore.status() != storage::Keystore::Status::Error) {
    replyError(ctx, err::kNotInitialized, "there is no wallet to erase");
    return;
  }
  beginPending(Pending::Erase, ctx, s_.config.reviewTimeoutS);
  startErase();
}

void Controller::startErase() {
  pages_ = {{"ERASE WALLET?", "ALL KEYS DELETED", true},
            {"MOVE FUNDS FIRST", "12 WORDS RESTORE", true},
            {"HOLD R TO ERASE", "L: KEEP WALLET", false}};
  seen_.assign(pages_.size(), false);
  pageIndex_ = 0;
  mode_ = Mode::Erase;
  hint_.clear();
  menuActivity_ = s_.clock.nowMs();
  sessionStart_ = menuActivity_;
  base_ = {pages_[0].top, pages_[0].bottom, ui::colors::kHigh};
  s_.buzzer.longTone();
}

void Controller::onEraseButton(const ui::ButtonEvent& ev) {
  using Kind = ui::ButtonEvent::Kind;
  menuActivity_ = s_.clock.nowMs();
  if (ev.kind == Kind::Press && ev.button == ui::Button::Left) {
    if (pending_ == Pending::Erase) finishPendingError(err::kRejected, "erase rejected on Aurora");
    clearSecrets();
    showNotice("WALLET KEPT", "", ui::colors::kIdle);
  } else if (ev.kind == Kind::Press && ev.button == ui::Button::Middle) {
    pageIndex_ = (pageIndex_ + 1) % pages_.size();
    hint_.clear();
    base_ = {pages_[pageIndex_].top, pages_[pageIndex_].bottom, ui::colors::kHigh};
  } else if (ev.kind == Kind::Hold && ev.button == ui::Button::Right && holdStartedInSession(ev) &&
             s_.screen.healthy()) {
    for (size_t i = 0; i < pages_.size(); ++i) {
      if (pages_[i].required && !seen_[i]) {
        hint_ = "M: SEE ALL PAGES";
        hintUntil_ = s_.clock.nowMs() + 1500;
        return;
      }
    }
    eraseWallet();
  }
}

void Controller::eraseWallet() {
  std::string why;
  const bool ok = s_.keystore.wipe(why);
  std::string ignored;
  s_.recipients.clear(ignored);
  balances_.clear();
  pinFailures_ = 0;
  clearSecrets();
  if (!ok) {
    LOG_ERROR("erase failed: %s", why.c_str());
    if (pending_ == Pending::Erase) finishPendingError(err::kStorageError, why);
    showNotice("ERASE FAILED", "SEE DEVICE LOGS", ui::colors::kHigh, 4000);
    return;
  }
  LOG_INFO("wallet erased on the device");
  if (pending_ == Pending::Erase) finishPending({{"ok", true}, {"erased", true}});
  showNotice("WALLET ERASED", "SET UP IN APP", ui::colors::kSetup, 4000);
}

}  // namespace wfw::device
