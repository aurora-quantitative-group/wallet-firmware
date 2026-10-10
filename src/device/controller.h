#pragma once

#include <functional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "config.h"
#include "crypto/keys.h"
#include "crypto/secure_channel.h"
#include "policy/risk.h"
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
  ui::ScreenRenderer& screen;
  ui::BuzzerController& buzzer;
  const ui::ButtonInput& buttons;
  const Clock& clock;
  const Config& config;
  // Entropy source for new wallets (randombytes_buf in production).
  std::function<void(uint8_t*, size_t)> randomBytes;
};

// The device state machine: answers serial requests, runs the on-device
// prompts (seed words, PIN, transaction review) and enforces that nothing is
// signed without a 2-second hold of the right button on a screen the user
// has seen. Single-threaded: call from one loop.
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
  enum class Pending { None, Setup, ShowWords, Sign, Handshake };
  enum class Mode { Idle, Notice, Words, Pin, Review };
  enum class PinPurpose { Set, Confirm, Unlock };

  struct Page {
    std::string top;
    std::string bottom;
    bool required = false;
  };

  // controller.cpp: transport, dispatch, simple commands, idle/notice.
  void dispatch(const json& req, bool encrypted);
  void reply(const Ctx& ctx, json body);
  void replyError(const Ctx& ctx, const char* code, const std::string& message = "");
  void finishPending(json body);
  void finishPendingError(const char* code, const std::string& message = "");
  void handlePing(const Ctx& ctx);
  void handleGetPubkey(const Ctx& ctx);
  void handleCancel(const Ctx& ctx);
  void handleConfirmBackup(const Ctx& ctx, const json& req);
  void handleHandshake(const Ctx& ctx, const json& req);
  void completeHandshake();
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
  void handleShowWords(const Ctx& ctx);
  void showWord();
  void onWordsButton(const ui::ButtonEvent& ev);
  void finishWords();
  void createWallet(const std::string& pin);
  void startPin(PinPurpose purpose);
  void showPin();
  void onPinButton(const ui::ButtonEvent& ev);
  void submitPin();
  void requireUnlock(std::function<void()> then);
  void clearSecrets();

  // controller_sign.cpp: sign_tx validation, review pages, approval.
  void handleSignTx(const Ctx& ctx, const json& req);
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
  TimeMs noticeUntil_ = 0;
  ui::Screen noticeScreen_;
  std::string hint_;  // transient bottom-line override
  TimeMs hintUntil_ = 0;

  // Seed words (setup and show_words).
  crypto::Secret<crypto::kEntropyBytes> newEntropy_;
  crypto::Mnemonic mnemonic_;
  int wordIndex_ = -1;  // -1 = intro screen
  TimeMs lastWordShownAt_ = 0;

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

  // Handshake waiting for unlock.
  std::array<uint8_t, 32> handshakeClientKey_{};
};

}  // namespace wfw::device
