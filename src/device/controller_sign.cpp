// sign_tx: validation, clear-signing review pages and approval (Steps 6-7).
#include <algorithm>

#include "device/controller.h"
#include "device/protocol.h"
#include "solana/format.h"
#include "util/log.h"

namespace wfw::device {

namespace {

ui::Rgb colorFor(policy::RiskLevel level) {
  switch (level) {
    case policy::RiskLevel::Low: return ui::colors::kLow;
    case policy::RiskLevel::Medium: return ui::colors::kMedium;
    case policy::RiskLevel::High: return ui::colors::kHigh;
  }
  return ui::colors::kHigh;
}

constexpr size_t kMaxMemoPages = 6;
constexpr size_t kMaxAppReason = 64;

}  // namespace

void Controller::handleSignTx(const Ctx& ctx, const json& req) {  // req = params
  if (!checkReadyForPrompt(ctx)) return;
  const auto& ks = s_.keystore;
  if (ks.status() == storage::Keystore::Status::Error) {
    replyError(ctx, err::kStorageError, ks.lastError());
    return;
  }
  if (!ks.initialized()) {
    replyError(ctx, err::kNotInitialized, "run setup");
    return;
  }

  const auto msgField = req.find("message");
  std::optional<Bytes> message;
  if (msgField != req.end() && msgField->is_string()) {
    message = base64Decode(msgField->get<std::string>());
  }
  if (!message) {
    replyError(ctx, err::kInvalidRequest, "message must be base64 of the serialized message");
    return;
  }
  uint32_t index = 0;
  if (const auto value = req.find("address_index"); value != req.end()) {
    if (!value->is_number_unsigned() || value->get<uint64_t>() >= crypto::kHardened ||
        !ks.receiveIndexAllocated(value->get<uint32_t>())) {
      replyError(ctx, err::kWrongWallet);
      return;
    }
    index = value->get<uint32_t>();
  }
  PubKey self = ks.publicKey();
  if (const auto a = req.find("address"); a != req.end()) {
    const auto decoded = a->is_string() ? decodeAddress(a->get<std::string>()) : std::nullopt;
    if (!decoded || (index == 0 && *decoded != self)) {
      replyError(ctx, err::kWrongWallet);
      return;
    }
    self = *decoded;
  } else if (index != 0) {
    replyError(ctx, err::kWrongWallet);
    return;
  }
  // The app's level can only raise the device's own verdict; without one the
  // device rule decides alone.
  policy::RiskLevel appLevel = policy::RiskLevel::Low;
  if (const auto r = req.find("risk"); r != req.end()) {
    const auto parsed = r->is_string() ? policy::parseRiskLevel(r->get<std::string>()) : std::nullopt;
    if (!parsed) {
      replyError(ctx, err::kInvalidRequest, "risk must be low, medium or high");
      return;
    }
    appLevel = *parsed;
  }
  std::string appReason;
  if (const auto r = req.find("reason"); r != req.end() && r->is_string()) {
    appReason = r->get<std::string>().substr(0, kMaxAppReason);
  }

  solana::Message msg;
  std::string why;
  if (!solana::parseMessage(message->data(), message->size(), msg, why)) {
    replyError(ctx, err::kInvalidMessage, why);
    return;
  }
  bool signer = false;
  for (size_t i = 0; i < msg.numRequiredSignatures; ++i) signer |= msg.accountKeys[i] == self;
  if (!signer) {
    replyError(ctx, err::kNotSigner, "this device's key is not a required signer");
    return;
  }

  const solana::TxSummary tx = solana::analyze(msg, self);
  policy::RiskPolicy riskPolicy;
  riskPolicy.newRecipientLimitLamports = s_.config.riskLimitLamports;
  riskPolicy.maxPriorityFeeLamports = s_.config.maxPriorityFeeLamports;
  risk_ = policy::assess(
      tx, appLevel, appReason, [this](const PubKey& k) { return s_.recipients.contains(k); }, self,
      riskPolicy);

  reviewIndex_ = index;
  reviewSelf_ = self;
  reviewMessage_ = std::move(*message);
  reviewRecipients_.clear();
  for (const auto& t : tx.transfers) {
    if (t.to != self && std::find(reviewRecipients_.begin(), reviewRecipients_.end(), t.to) ==
                            reviewRecipients_.end()) {
      reviewRecipients_.push_back(t.to);
    }
  }
  blindOnly_ = !tx.fullyRecognised() && !s_.config.allowBlindSigning;
  // A blind-signed transaction never makes its recipients "known".
  if (!tx.fullyRecognised()) reviewRecipients_.clear();
  buildReviewPages(tx);
  LOG_INFO("sign_tx: %zu transfer(s), %zu unknown ix, app=%s device=%s -> %s (%s)",
           tx.transfers.size(), tx.unknown.size(), policy::toString(appLevel),
           policy::toString(risk_.deviceLevel), policy::toString(risk_.level),
           risk_.reason.c_str());

  beginPending(Pending::Sign, ctx, s_.config.reviewTimeoutS);
  requireUnlock([this] {
    crypto::KeyPair child;
    if (!s_.keystore.solanaKey(reviewIndex_, child) || child.publicKey != reviewSelf_) {
      finishPendingError(err::kWrongWallet);
      clearSecrets();
      showIdle();
      return;
    }
    startReview();
  });
}

void Controller::buildReviewPages(const solana::TxSummary& tx) {
  pages_.clear();
  // The strongest warning comes first.
  for (const auto& u : tx.unknown) {
    pages_.push_back({"UNKNOWN TX", u.reason, true});
    if (u.program) {
      pages_.push_back({"PROGRAM:", abbreviateAddress(base58Encode(*u.program)), false});
    }
  }

  const size_t n = tx.transfers.size();
  if (n > 1) {
    uint64_t total = 0;
    for (const auto& t : tx.transfers) {
      total = (UINT64_MAX - total < t.lamports) ? UINT64_MAX : total + t.lamports;
    }
    const std::string sum = "TOTAL " + formatSol(total) + " SOL";
    pages_.push_back({std::to_string(n) + " TRANSFERS",
                      sum.size() <= kLcdCols ? sum : formatSol(total), true});
  }
  for (const auto& t : tx.transfers) {
    const std::string amount = formatSol(t.lamports);
    const std::string address = base58Encode(t.to);
    const std::string line = "SEND " + amount + " SOL";
    if (line.size() <= kLcdCols) {
      pages_.push_back({line, "TO " + abbreviateAddress(address), true});
    } else {  // never truncate an amount: give it a line (or two) of its own
      if (amount.size() + 4 <= kLcdCols) {
        pages_.push_back({"SEND", amount + " SOL", true});
      } else if (amount.size() <= kLcdCols) {
        pages_.push_back({"SEND SOL", amount, true});
      } else {
        pages_.push_back({amount.substr(0, kLcdCols), amount.substr(kLcdCols) + " SOL", true});
      }
      pages_.push_back({"TO " + abbreviateAddress(address), "", true});
    }
    // Middle scrolls through the full address. For a first-time recipient it
    // must be read in full: 4+4 characters are easy to imitate (address poisoning).
    const bool isNew = std::find(risk_.newRecipients.begin(), risk_.newRecipients.end(), t.to) !=
                       risk_.newRecipients.end();
    pages_.push_back({address.substr(0, 16), address.size() > 16 ? address.substr(16, 16) : "",
                      isNew});
    if (address.size() > 32) pages_.push_back({address.substr(32), "(FULL ADDRESS)", isNew});
  }
  if (n == 0 && tx.unknown.empty()) pages_.push_back({"SIGN TX", "NO SOL TRANSFER", true});

  // Every memo page is labelled on the top line, so memo text can never pose
  // as a firmware screen (e.g. a fake "RISK: LOW").
  for (const auto& memo : tx.memos) {
    const std::string text = lcdSanitize(memo);
    const size_t chunks = std::max<size_t>(1, (text.size() + kLcdCols - 1) / kLcdCols);
    const size_t shown = std::min(chunks, kMaxMemoPages);
    for (size_t i = 0; i < shown; ++i) {
      std::string top = "MEMO " + std::to_string(i + 1) + "/" + std::to_string(shown);
      if (i + 1 == shown && chunks > shown) top += " (CUT)";
      pages_.push_back({top, text.substr(i * kLcdCols, kLcdCols), i == 0});
    }
  }
  if (tx.maxPriorityFeeLamports > 0) {
    const std::string fee = "<=" + formatSol(tx.maxPriorityFeeLamports) + " SOL";
    pages_.push_back({"PRIORITY FEE",
                      fee.size() <= kLcdCols ? fee : formatSol(tx.maxPriorityFeeLamports), true});
  }
  // A yellow or red screen must explain itself before it can be approved:
  // the colour alone is easy to miss, and some boards have no backlight.
  pages_.push_back({std::string("RISK: ") + policy::toLabel(risk_.level), risk_.reason,
                    risk_.level > policy::RiskLevel::Low});
  pages_.push_back(blindOnly_ ? Page{"CANNOT SIGN", "L: REJECT", false}
                              : Page{"HOLD R TO SIGN", "L:REJECT R:NEXT", false});
}

void Controller::startReview() {
  mode_ = Mode::Review;
  pageIndex_ = 0;
  seen_.assign(pages_.size(), false);
  sessionStart_ = s_.clock.nowMs();  // a hold that began earlier never counts
  showReviewPage();
  if (risk_.level == policy::RiskLevel::High) {
    s_.buzzer.longTone();
  } else {
    s_.buzzer.beep();
  }
}

void Controller::showReviewPage() {
  mode_ = Mode::Review;
  hint_.clear();  // the user moved on; show the page itself
  // render() marks the page seen once it has really been drawn.
  const Page& p = pages_[pageIndex_];
  base_ = {p.top, p.bottom, colorFor(risk_.level)};
}

void Controller::onReviewButton(const ui::ButtonEvent& ev) {
  using Kind = ui::ButtonEvent::Kind;
  if (ev.kind == Kind::Press && ev.button == ui::Button::Left) {
    reject();
  } else if (ev.kind == Kind::Press && ev.button == ui::Button::Middle) {
    pageIndex_ = (pageIndex_ + 1) % pages_.size();
    showReviewPage();
  } else if (ev.kind == Kind::Hold && ev.button == ui::Button::Right) {
    if (!holdStartedInSession(ev) || !s_.screen.healthy()) return;
    std::string blocker;
    if (blindOnly_) {
      blocker = "CANNOT SIGN";
    } else {
      for (size_t i = 0; i < pages_.size(); ++i) {
        if (pages_[i].required && !seen_[i]) blocker = "R: SEE ALL PAGES";
      }
    }
    if (!blocker.empty()) {
      hint_ = blocker;
      hintUntil_ = s_.clock.nowMs() + 1500;
      return;
    }
    approve();
  }
}

void Controller::approve() {
  crypto::KeyPair child;
  const crypto::KeyPair* kp = s_.keystore.keyPair();
  if (reviewIndex_ != 0) {
    kp = s_.keystore.solanaKey(reviewIndex_, child) && child.publicKey == reviewSelf_
             ? &child : nullptr;
  }
  if (kp == nullptr) {
    finishPendingError(err::kInternal, "keystore locked");
    clearSecrets();
    showIdle();
    return;
  }
  uint8_t sig[crypto::kSignatureBytes];
  crypto::signDetached(*kp, reviewMessage_.data(), reviewMessage_.size(), sig);
  std::string why;
  if (!s_.recipients.addAll(reviewRecipients_, why)) {
    LOG_WARN("could not save known recipients: %s", why.c_str());
  }
  LOG_INFO("sign_tx: approved on device");
  finishPending({{"ok", true},
                 {"signature", base64Encode(sig, sizeof(sig))},
                 {"risk", policy::toString(risk_.level)},
                 {"reason", risk_.reason}});
  clearSecrets();
  showNotice("SIGNED", "", ui::colors::kLow);
  s_.buzzer.beep();
}

void Controller::reject() {
  LOG_INFO("sign_tx: rejected on device");
  finishPending({{"ok", false},
                 {"error", err::kRejected},
                 {"risk", policy::toString(risk_.level)},
                 {"reason", risk_.reason}});
  clearSecrets();
  showNotice("REJECTED", "", ui::colors::kIdle);
}

}  // namespace wfw::device
