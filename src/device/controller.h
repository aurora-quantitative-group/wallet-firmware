#pragma once

#include <functional>
#include <map>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "config.h"
#include "crypto/keys.h"
#include "crypto/secure_channel.h"
#include "policy/risk.h"
#include "storage/identity.h"
#include "storage/keystore.h"
#include "storage/recipients.h"
#include "ui/button_input.h"
#include "ui/buzzer.h"
#include "ui/screen.h"
#include "util/clock.h"

namespace wfw::device {

// Everything the controller drives. All hardware sits behind interfaces, so
// the whole device behaviour runs unchanged in unit tests and the simulator.
struct Services {
  storage::Keystore& keystore;
  storage::KnownRecipients& recipients;
  const storage::DeviceIdentity& identity;
  ui::ScreenRenderer& screen;
  ui::BuzzerController& buzzer;
  const ui::ButtonInput& buttons;
  const Clock& clock;
  const Config& config;
  // Entropy source for new wallets (randombytes_buf in production).
  std::function<void(uint8_t*, size_t)> randomBytes;
};

// The Aurora device state machine: answers serial requests (protocol v2,
// docs/firmware/protocol-contract.md), runs the on-device prompts (seed words
// and their check, PIN, pairing, transaction review) and enforces that
// nothing is signed without a 2-second hold of the right button on a screen
// the user has seen. Single-threaded: call from one loop.
class Controller {
 public:
  using Sender = std::function<void(const std::string& line)>;

  Controller(Services services, Sender send);

  void start();
  void onLine(const std::string& line);
  void onOverflow();
  void onButton(const ui::ButtonEvent& ev);
  void tick();
  // The serial link was reopened: drop the secure channel and any prompt
  // whose answer can no longer be delivered.
  void onLinkReset();

  bool sessionActive() const { return pending_ != Pending::None; }

 private:
  using json = nlohmann::json;

  struct Ctx {
    json id;
    bool encrypted = false;
  };
  enum class Pending { None, Setup, ShowSeed, Sign, Pair, Erase };
  enum class Mode { Idle, Notice, Words, Quiz, Pin, Review, Pair, Menu, Browse, Erase };
  enum class PinPurpose { Set, Confirm, Unlock };

  struct Page {
    std::string top;
    std::string bottom;
    bool required = false;
  };

  // controller.cpp: transport, dispatch, simple commands, idle/notice.
  void dispatch(const json& req, bool encrypted);
  static std::string humanError(const std::string& code);
  void reply(const Ctx& ctx, json body);
  void replyError(const Ctx& ctx, const char* code, const std::string& message = "");
  void finishPending(json body);
  void finishPendingError(const char* code, const std::string& message = "");
  void handlePing(const Ctx& ctx, const json& params);
  void handleGetPubkey(const Ctx& ctx);
  void handleCancel(const Ctx& ctx);
  void handleHandshake(const Ctx& ctx, const json& params);
  void handlePair(const Ctx& ctx, const json& params);
  void onPairButton(const ui::ButtonEvent& ev);
  json signedPayload(const json& payload) const;
  json walletManifest() const;
  bool checkReadyForPrompt(const Ctx& ctx);
  void beginPending(Pending kind, const Ctx& ctx, int timeoutS);
  void showIdle();
  void showNotice(const std::string& top, const std::string& bottom, ui::Rgb color,
                  TimeMs durationMs = 1500);
  void render();
  std::string deviceName() const;
  std::string holdBar() const;
  bool holdStartedInSession(const ui::ButtonEvent& ev) const;

  // controller_setup.cpp: seed words, backup re-show, PIN entry.
  void handleSetup(const Ctx& ctx);
  void handleShowSeed(const Ctx& ctx);
  void showWord();
  void onWordsButton(const ui::ButtonEvent& ev);
  void startQuiz();
  void prepareChoices();
  void showQuiz();
  void onQuizButton(const ui::ButtonEvent& ev);
  void finishBackupCheck();
  void createWallet(const std::string& pin);
  void startPin(PinPurpose purpose);
  void showPin();
  void onPinButton(const ui::ButtonEvent& ev);
  void submitPin();
  void requireUnlock(std::function<void()> then);
  void clearSecrets();

  // controller_menu.cpp: on-device menu, wallet list, erase, app kill switch.
  void handleReset(const Ctx& ctx);
  void handleBalances(const Ctx& ctx, const json& params);
  std::vector<std::string> menuItems() const;
  void openMenu();
  void showMenu();
  void onMenuButton(const ui::ButtonEvent& ev);
  void showWalletPage();
  void onBrowseButton(const ui::ButtonEvent& ev);
  void setAppDisconnected(bool disconnected);
  void startErase();
  void onEraseButton(const ui::ButtonEvent& ev);
  void eraseWallet();

  // controller_sign.cpp: sign_tx validation, review pages, approval.
  void handleSignTx(const Ctx& ctx, const json& params);
  void buildReviewPages(const solana::TxSummary& tx);
  void startReview();
  void showReviewPage();
  void onReviewButton(const ui::ButtonEvent& ev);
  void approve();
  void reject();

  Services s_;
  Sender send_;
  crypto::SecureChannel channel_{crypto::SecureChannel::Role::Device};

  Mode mode_ = Mode::Idle;
  ui::Screen base_;  // screen for the current mode, before hint/hold overlays
  Pending pending_ = Pending::None;
  Ctx pendingCtx_;
  TimeMs pendingDeadline_ = 0;
  TimeMs sessionStart_ = 0;  // a confirming hold must start after this
  TimeMs lastActivity_ = 0;  // for auto-lock
  TimeMs noticeUntil_ = 0;
  ui::Screen noticeScreen_;
  std::string hint_;  // transient bottom-line override
  TimeMs hintUntil_ = 0;

  // Seed words (setup and show_seed) and the on-device backup check.
  crypto::Secret<crypto::kEntropyBytes> newEntropy_;
  crypto::Mnemonic mnemonic_;
  int wordIndex_ = -1;  // -1 = intro screen
  TimeMs lastWordShownAt_ = 0;
  static constexpr int kQuizQuestions = 3;
  static constexpr int kQuizChoices = 4;
  std::array<int, kQuizQuestions> quizPositions_{};  // word positions asked
  std::array<int, kQuizChoices> quizChoices_{};      // BIP-39 wordlist indices
  int quizStep_ = 0;
  int quizChoice_ = 0;  // highlighted candidate

  // Device menu and wallet list. Balances come from the app (informational).
  struct Balance {
    std::string amount;
    std::string symbol;
  };
  std::map<std::string, Balance> balances_;
  bool appDisconnected_ = false;  // kill switch from the device menu
  size_t menuIndex_ = 0;
  size_t browseIndex_ = 0;
  TimeMs menuActivity_ = 0;

  // Pairing prompt.
  std::string pairChallenge_;
  std::string pairCode_;

  // PIN entry.
  PinPurpose pinPurpose_ = PinPurpose::Unlock;
  std::string pinDigits_;
  std::string firstPin_;
  int pinDigit_ = 0;
  int pinFailures_ = 0;
  std::function<void()> afterUnlock_;

  // Transaction review.
  Bytes reviewMessage_;
  std::vector<Page> pages_;
  std::vector<bool> seen_;
  size_t pageIndex_ = 0;
  policy::RiskResult risk_;
  std::vector<PubKey> reviewRecipients_;
  bool blindOnly_ = false;  // unknown instructions and blind signing disabled
};

}  // namespace wfw::device
