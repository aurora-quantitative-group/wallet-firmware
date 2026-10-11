// Seed words, the on-device backup check (setup / show_seed) and PIN entry.
#include <sodium.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

extern "C" {
#include "bip39.h"
}

#include "device/controller.h"
#include "device/protocol.h"
#include "solana/format.h"
#include "util/log.h"

namespace wfw::device {

namespace {

void wipeString(std::string& s) {
  if (!s.empty()) sodium_memzero(s.data(), s.size());
  s.clear();
}

constexpr int kLastWord = static_cast<int>(crypto::kMnemonicWords) - 1;

}  // namespace

void Controller::handleSetup(const Ctx& ctx) {
  if (!checkReadyForPrompt(ctx)) return;
  switch (s_.keystore.status()) {
    case storage::Keystore::Status::Error:
      replyError(ctx, err::kStorageError, s_.keystore.lastError());
      return;
    case storage::Keystore::Status::Locked:
    case storage::Keystore::Status::Ready:
      replyError(ctx, err::kAlreadyInitialized, "this device already holds a wallet");
      return;
    case storage::Keystore::Status::Empty:
      break;
  }
  // Step 5: 128 bits from libsodium's CSPRNG -> 12-word BIP-39 mnemonic.
  s_.randomBytes(newEntropy_.data(), newEntropy_.size());
  if (!mnemonic_.fromEntropy(newEntropy_.data(), newEntropy_.size())) {
    clearSecrets();
    replyError(ctx, err::kInternal, "mnemonic generation failed");
    return;
  }
  LOG_INFO("setup: showing new recovery words");
  beginPending(Pending::Setup, ctx, s_.config.setupTimeoutS);
  wordIndex_ = -1;
  showWord();
  s_.buzzer.beep();
}

// Re-shows the words of a wallet whose backup was never checked, then runs
// the same on-device check. Once confirmed, the words are never shown again.
void Controller::handleShowSeed(const Ctx& ctx) {
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
  // Once the backup is confirmed the words never leave storage again.
  if (ks.backupConfirmed()) {
    replyError(ctx, err::kBackupAlreadyConfirmed);
    return;
  }
  beginPending(Pending::ShowSeed, ctx, s_.config.setupTimeoutS);
  requireUnlock([this] {
    if (!s_.keystore.mnemonic(mnemonic_)) {
      finishPendingError(err::kInternal, "could not rebuild the recovery words");
      showIdle();
      return;
    }
    sessionStart_ = s_.clock.nowMs();
    wordIndex_ = -1;
    showWord();
    s_.buzzer.beep();
  });
}

void Controller::showWord() {
  mode_ = Mode::Words;
  hint_.clear();
  lastWordShownAt_ = s_.clock.nowMs();
  if (wordIndex_ < 0) {
    base_ = {"WRITE DOWN THE", "12 WORDS   M:GO", ui::colors::kSetup};
    return;
  }
  char word[16] = {0};
  char top[32] = {0};
  mnemonic_.word(static_cast<size_t>(wordIndex_), word, sizeof(word));
  std::snprintf(top, sizeof(top), "%02d/%02zu %s", wordIndex_ + 1, mnemonic_.wordCount(), word);
  const char* bottom = wordIndex_ == 0          ? "M:NEXT"
                       : wordIndex_ < kLastWord ? "L:BACK  M:NEXT"
                                                : "L:BACK HOLD R:OK";
  base_ = {top, bottom, ui::colors::kSetup};
  sodium_memzero(word, sizeof(word));
  sodium_memzero(top, sizeof(top));
}

void Controller::onWordsButton(const ui::ButtonEvent& ev) {
  using Kind = ui::ButtonEvent::Kind;
  if (ev.kind == Kind::Press && ev.button == ui::Button::Left && wordIndex_ >= 0) {
    --wordIndex_;
    showWord();
  } else if (ev.kind == Kind::Press && ev.button == ui::Button::Middle && wordIndex_ < kLastWord) {
    ++wordIndex_;
    showWord();
  } else if (ev.kind == Kind::Press && ev.button == ui::Button::Right && wordIndex_ < kLastWord) {
    hint_ = "GO TO WORD 12";
    hintUntil_ = s_.clock.nowMs() + 1500;
  } else if (ev.kind == Kind::Hold && ev.button == ui::Button::Right && wordIndex_ == kLastWord &&
             ev.pressedAt >= lastWordShownAt_) {
    startQuiz();
  }
}

// ------------------------------------------------- on-device backup check --
// The user proves the backup on Aurora itself: for 3 random positions they
// pick the right word out of 4 (M/L cycle, hold R to choose). The phrase
// never goes to the app. A wrong answer sends them back to the words.

namespace {

void wordAt(const crypto::Mnemonic& m, int position, char (&out)[16]) {
  std::memset(out, 0, sizeof(out));
  m.word(static_cast<size_t>(position), out, sizeof(out));
}

}  // namespace

void Controller::startQuiz() {
  for (int q = 0; q < kQuizQuestions; ++q) {
    int pos = 0;
    do {
      pos = static_cast<int>(randombytes_uniform(crypto::kMnemonicWords));
    } while (std::find(quizPositions_.begin(), quizPositions_.begin() + q, pos) !=
             quizPositions_.begin() + q);
    quizPositions_[q] = pos;
  }
  std::sort(quizPositions_.begin(), quizPositions_.end());
  quizStep_ = 0;
  prepareChoices();
  showQuiz();
}

void Controller::prepareChoices() {
  char word[16];
  wordAt(mnemonic_, quizPositions_[quizStep_], word);
  const int correct = mnemonic_find_word(word).index;
  sodium_memzero(word, sizeof(word));
  quizChoices_[0] = correct;
  for (int i = 1; i < kQuizChoices; ++i) {
    int candidate = 0;
    do {
      candidate = static_cast<int>(randombytes_uniform(BIP39_WORD_COUNT));
    } while (std::find(quizChoices_.begin(), quizChoices_.begin() + i, candidate) !=
             quizChoices_.begin() + i);
    quizChoices_[i] = candidate;
  }
  std::swap(quizChoices_[0], quizChoices_[randombytes_uniform(kQuizChoices)]);
  quizChoice_ = 0;
}

void Controller::showQuiz() {
  mode_ = Mode::Quiz;
  hint_.clear();
  sessionStart_ = s_.clock.nowMs();  // a choosing hold must start on this candidate
  char top[48];
  char bottom[48];
  std::snprintf(top, sizeof(top), "WORD #%02d? M:NEXT", quizPositions_[quizStep_] + 1);
  std::snprintf(bottom, sizeof(bottom), "%-12s%d/%d", mnemonic_get_word(quizChoices_[quizChoice_]),
                quizChoice_ + 1, kQuizChoices);
  base_ = {top, bottom, ui::colors::kSetup};
}

void Controller::onQuizButton(const ui::ButtonEvent& ev) {
  using Kind = ui::ButtonEvent::Kind;
  if (ev.kind == Kind::Press && ev.button == ui::Button::Middle) {
    quizChoice_ = (quizChoice_ + 1) % kQuizChoices;
    showQuiz();
  } else if (ev.kind == Kind::Press && ev.button == ui::Button::Left) {
    quizChoice_ = (quizChoice_ + kQuizChoices - 1) % kQuizChoices;
    showQuiz();
  } else if (ev.kind == Kind::Hold && ev.button == ui::Button::Right && holdStartedInSession(ev) &&
             s_.screen.healthy()) {
    char word[16];
    wordAt(mnemonic_, quizPositions_[quizStep_], word);
    const bool right = std::strcmp(word, mnemonic_get_word(quizChoices_[quizChoice_])) == 0;
    sodium_memzero(word, sizeof(word));
    if (!right) {
      LOG_INFO("backup check: wrong word, showing the words again");
      wordIndex_ = -1;
      showWord();
      hint_ = "WRONG WORD! M:GO";
      hintUntil_ = s_.clock.nowMs() + 3000;
      return;
    }
    if (++quizStep_ < kQuizQuestions) {
      prepareChoices();
      showQuiz();
      return;
    }
    finishBackupCheck();
  }
}

void Controller::finishBackupCheck() {
  LOG_INFO("backup check passed on device");
  if (pending_ == Pending::ShowSeed) {
    mnemonic_.wipe();
    std::string why;
    if (!s_.keystore.setBackupConfirmed(why)) {
      finishPendingError(err::kStorageError, why);
      showNotice("SAVE FAILED", "SEE DEVICE LOGS", ui::colors::kHigh, 4000);
      return;
    }
    finishPending({{"ok", true}, {"shown", true}, {"backup_confirmed", true}});
    showNotice("BACKUP CHECKED", "", ui::colors::kLow, 2500);
    return;
  }
  if (s_.config.pinEnabled) {
    startPin(PinPurpose::Set);
  } else {
    createWallet("");
  }
}

void Controller::createWallet(const std::string& pin) {
  base_ = {"SAVING WALLET", "PLEASE WAIT", ui::colors::kSetup};
  render();  // Argon2id blocks for a moment; show why
  std::string why;
  const bool ok = s_.keystore.create(newEntropy_.data(), pin, why, /*backupConfirmed=*/true);
  clearSecrets();
  if (!ok) {
    LOG_ERROR("setup: could not store wallet: %s", why.c_str());
    finishPendingError(err::kStorageError, why);
    showNotice("SETUP FAILED", "SEE DEVICE LOGS", ui::colors::kHigh, 4000);
    return;
  }
  LOG_INFO("setup: wallet created, address %s", s_.keystore.address().c_str());
  finishPending({{"ok", true},
                 {"created", true},
                 {"shown", true},
                 {"has_seed", true},
                 {"backup_confirmed", true},
                 {"pubkey", s_.keystore.address()},
                 {"pin_protected", s_.keystore.pinProtected()}});
  showNotice("WALLET CREATED", abbreviateAddress(s_.keystore.address()), ui::colors::kLow, 3000);
}

// --------------------------------------------------------------------- PIN --

void Controller::requireUnlock(std::function<void()> then) {
  if (s_.keystore.ready()) {
    then();
    return;
  }
  afterUnlock_ = std::move(then);
  startPin(PinPurpose::Unlock);
}

void Controller::startPin(PinPurpose purpose) {
  pinPurpose_ = purpose;
  wipeString(pinDigits_);
  pinDigit_ = 0;
  showPin();
}

void Controller::showPin() {
  mode_ = Mode::Pin;
  std::string top;
  switch (pinPurpose_) {
    case PinPurpose::Set: top = "SET NEW PIN"; break;
    case PinPurpose::Confirm: top = "REPEAT NEW PIN"; break;
    case PinPurpose::Unlock:
      top = "ENTER PIN";
      if (pinFailures_ > 0) {  // "ENTER PIN 3 LEFT" fits 16 columns
        top += " " + std::to_string(s_.config.pinMaxAttempts - pinFailures_) + " LEFT";
      }
      break;
  }
  const size_t len = static_cast<size_t>(s_.config.pinLength);
  std::string masked(pinDigits_.size(), '*');
  masked.push_back(static_cast<char>('0' + pinDigit_));
  if (masked.size() < len) masked.append(len - masked.size(), '_');
  base_ = {top, masked + " -L +R M", ui::colors::kSetup};
}

void Controller::onPinButton(const ui::ButtonEvent& ev) {
  if (ev.kind != ui::ButtonEvent::Kind::Press) return;
  switch (ev.button) {
    case ui::Button::Left: pinDigit_ = (pinDigit_ + 9) % 10; break;
    case ui::Button::Right: pinDigit_ = (pinDigit_ + 1) % 10; break;
    case ui::Button::Middle:
      pinDigits_.push_back(static_cast<char>('0' + pinDigit_));
      pinDigit_ = 0;
      if (pinDigits_.size() >= static_cast<size_t>(s_.config.pinLength)) {
        submitPin();
        return;
      }
      break;
  }
  showPin();
}

void Controller::submitPin() {
  switch (pinPurpose_) {
    case PinPurpose::Set:
      wipeString(firstPin_);
      firstPin_.swap(pinDigits_);
      startPin(PinPurpose::Confirm);
      return;
    case PinPurpose::Confirm: {
      const bool same = pinDigits_.size() == firstPin_.size() &&
                        sodium_memcmp(pinDigits_.data(), firstPin_.data(), firstPin_.size()) == 0;
      if (!same) {
        wipeString(firstPin_);
        startPin(PinPurpose::Set);
        hint_ = "PIN MISMATCH";
        hintUntil_ = s_.clock.nowMs() + 2000;
        return;
      }
      std::string pin;
      pin.swap(firstPin_);
      wipeString(pinDigits_);
      createWallet(pin);
      wipeString(pin);
      return;
    }
    case PinPurpose::Unlock: {
      base_ = {"CHECKING PIN", "PLEASE WAIT", ui::colors::kSetup};
      render();
      std::string why;
      const bool ok = s_.keystore.unlock(pinDigits_, why);
      wipeString(pinDigits_);
      if (ok) {
        pinFailures_ = 0;
        if (!s_.keystore.setPinFailures(0, why)) LOG_WARN("pin counter: %s", why.c_str());
        LOG_INFO("keystore unlocked");
        auto then = std::move(afterUnlock_);
        afterUnlock_ = nullptr;
        if (then) then();
        return;
      }
      if (why != "wrong PIN") {
        LOG_ERROR("unlock failed: %s", why.c_str());
        finishPendingError(err::kInternal, why);
        clearSecrets();
        showNotice("UNLOCK FAILED", "SEE DEVICE LOGS", ui::colors::kHigh, 4000);
        return;
      }
      ++pinFailures_;
      // Persist before saying anything, so power-cycling cannot reset the count.
      if (!s_.keystore.setPinFailures(pinFailures_, why)) {
        LOG_ERROR("pin counter: %s", why.c_str());
      }
      LOG_WARN("wrong PIN (%d of %d)", pinFailures_, s_.config.pinMaxAttempts);
      if (pinFailures_ >= s_.config.pinMaxAttempts) {
        finishPendingError(err::kPinLocked, "too many wrong PINs; restore from the recovery words");
        clearSecrets();
        showNotice("TOO MANY TRIES", "WALLET LOCKED", ui::colors::kHigh, 5000);
        return;
      }
      startPin(PinPurpose::Unlock);
      hint_ = "WRONG PIN";
      hintUntil_ = s_.clock.nowMs() + 1500;
      return;
    }
  }
}

void Controller::clearSecrets() {
  mnemonic_.wipe();
  newEntropy_.wipe();
  wipeString(pinDigits_);
  wipeString(firstPin_);
  pinDigit_ = 0;
  wordIndex_ = -1;
  quizPositions_.fill(0);
  quizChoices_.fill(0);
  quizStep_ = 0;
  quizChoice_ = 0;
  pairChallenge_.clear();
  pairCode_.clear();
  afterUnlock_ = nullptr;
  reviewMessage_.clear();
  pages_.clear();
  seen_.clear();
  reviewRecipients_.clear();
  hint_.clear();
}

}  // namespace wfw::device
