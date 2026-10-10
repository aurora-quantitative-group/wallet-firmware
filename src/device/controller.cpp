#include "device/controller.h"

#include <sodium.h>

#include "device/protocol.h"
#include "solana/format.h"
#include "util/log.h"

namespace wfw::device {

Controller::Controller(Services services, Sender send)
    : s_(std::move(services)), send_(std::move(send)) {
  if (!s_.randomBytes) s_.randomBytes = [](uint8_t* b, size_t n) { randombytes_buf(b, n); };
}

void Controller::start() {
  showIdle();
  render();
}

// ------------------------------------------------------------- transport --

void Controller::onLine(const std::string& line) {
  json req = json::parse(line, nullptr, false);
  if (req.is_discarded() || !req.is_object()) {
    replyError(Ctx{}, err::kInvalidJson, "each line must be one JSON object");
    return;
  }
  const auto enc = req.find("enc");
  if (enc == req.end()) {
    dispatch(req, false);
    return;
  }
  if (!enc->is_string()) {
    replyError(Ctx{}, err::kInvalidRequest, "enc must be a base64 string");
    return;
  }
  if (!channel_.active()) {
    replyError(Ctx{}, err::kNoSecureChannel, "send handshake first");
    return;
  }
  std::string plain;
  std::string why;
  if (!channel_.open(enc->get<std::string>(), plain, why)) {
    LOG_WARN("secure channel: %s", why.c_str());
    replyError(Ctx{}, err::kDecryptFailed, why);
    return;
  }
  json inner = json::parse(plain, nullptr, false);
  sodium_memzero(plain.data(), plain.size());
  if (inner.is_discarded() || !inner.is_object()) {
    replyError(Ctx{json(), true}, err::kInvalidJson, "decrypted payload is not a JSON object");
    return;
  }
  dispatch(inner, true);
}

void Controller::onOverflow() {
  replyError(Ctx{}, err::kLineTooLong, "line exceeds " + std::to_string(kMaxLineBytes) + " bytes");
}

void Controller::dispatch(const json& req, bool encrypted) {
  Ctx ctx;
  ctx.encrypted = encrypted;
  if (const auto id = req.find("id"); id != req.end()) {
    if (!id->is_primitive()) {
      replyError(ctx, err::kInvalidRequest, "id must be a string or number");
      return;
    }
    ctx.id = *id;
  }
  const auto c = req.find("cmd");
  if (c == req.end() || !c->is_string()) {
    replyError(ctx, err::kInvalidRequest, "missing string field 'cmd'");
    return;
  }
  const std::string& name = c->get_ref<const std::string&>();
  LOG_DEBUG("request: %s%s", name.c_str(), encrypted ? " (encrypted)" : "");

  if (s_.config.requireSecureChannel && !encrypted && name != cmd::kPing &&
      name != cmd::kGetPubkey && name != cmd::kHandshake) {
    replyError(ctx, err::kSecureChannelRequired, "complete the handshake first");
    return;
  }
  if (name == cmd::kPing) {
    handlePing(ctx);
  } else if (name == cmd::kGetPubkey) {
    handleGetPubkey(ctx);
  } else if (name == cmd::kSetup) {
    handleSetup(ctx);
  } else if (name == cmd::kSignTx) {
    handleSignTx(ctx, req);
  } else if (name == cmd::kShowWords) {
    handleShowWords(ctx);
  } else if (name == cmd::kConfirmBackup) {
    handleConfirmBackup(ctx, req);
  } else if (name == cmd::kCancel) {
    handleCancel(ctx);
  } else if (name == cmd::kHandshake) {
    handleHandshake(ctx, req);
  } else {
    replyError(ctx, err::kUnknownCmd, "unknown cmd '" + lcdSanitize(name.substr(0, 32)) + "'");
  }
  render();
}

void Controller::reply(const Ctx& ctx, json body) {
  body["id"] = ctx.id;
  std::string text = body.dump(-1, ' ', false, json::error_handler_t::replace);
  if (ctx.encrypted) {
    if (!channel_.active()) {
      LOG_WARN("dropping encrypted reply: secure channel is gone");
      return;
    }
    text = json{{"enc", channel_.seal(text)}}.dump();
  }
  send_(text);
}

void Controller::replyError(const Ctx& ctx, const char* code, const std::string& message) {
  json body = {{"ok", false}, {"error", code}};
  if (!message.empty()) body["message"] = message;
  reply(ctx, std::move(body));
}

void Controller::finishPending(json body) {
  const Ctx ctx = pendingCtx_;
  pending_ = Pending::None;
  pendingCtx_ = Ctx{};
  reply(ctx, std::move(body));
}

void Controller::finishPendingError(const char* code, const std::string& message) {
  json body = {{"ok", false}, {"error", code}};
  if (!message.empty()) body["message"] = message;
  finishPending(std::move(body));
}

// ------------------------------------------------------------ session glue --

bool Controller::checkReadyForPrompt(const Ctx& ctx) {
  if (sessionActive()) {
    replyError(ctx, err::kBusy, "a device prompt is in progress");
    return false;
  }
  if (!s_.screen.healthy()) {
    replyError(ctx, err::kDisplayUnavailable, "the device display is not responding");
    return false;
  }
  if (s_.keystore.pinProtected() && !s_.keystore.ready() &&
      pinFailures_ >= s_.config.pinMaxAttempts) {
    replyError(ctx, err::kPinLocked, "too many wrong PINs; restart the device");
    return false;
  }
  return true;
}

void Controller::beginPending(Pending kind, const Ctx& ctx, int timeoutS) {
  pending_ = kind;
  pendingCtx_ = ctx;
  sessionStart_ = s_.clock.nowMs();
  pendingDeadline_ = sessionStart_ + static_cast<TimeMs>(timeoutS) * 1000;
  hint_.clear();
}

void Controller::onLinkReset() {
  channel_.reset();
  if (sessionActive()) {
    LOG_WARN("serial link reset: abandoning the pending prompt");
    pending_ = Pending::None;
    pendingCtx_ = Ctx{};
    clearSecrets();
    showNotice("DISCONNECTED", "PROMPT CANCELLED", ui::colors::kIdle);
  }
  if (s_.keystore.pinProtected()) s_.keystore.lock();
  render();
}

void Controller::onButton(const ui::ButtonEvent& ev) {
  switch (mode_) {
    case Mode::Words: onWordsButton(ev); break;
    case Mode::Pin: onPinButton(ev); break;
    case Mode::Review: onReviewButton(ev); break;
    case Mode::Notice:
      if (ev.kind == ui::ButtonEvent::Kind::Press && !sessionActive()) showIdle();
      break;
    case Mode::Idle: break;
  }
  render();
}

void Controller::tick() {
  const TimeMs now = s_.clock.nowMs();
  s_.buzzer.tick();
  s_.screen.retryIfNeeded();
  if (sessionActive() && now >= pendingDeadline_) {
    LOG_INFO("device prompt timed out");
    finishPendingError(err::kTimeout, "no answer on the device in time");
    clearSecrets();
    showNotice("TIMED OUT", "", ui::colors::kIdle);
  }
  if (mode_ == Mode::Notice && now >= noticeUntil_) showIdle();
  if (!hint_.empty() && now >= hintUntil_) hint_.clear();
  render();
}

// ----------------------------------------------------------------- screens --

std::string Controller::deviceName() const {
  if (!s_.config.deviceName.empty()) return s_.config.deviceName;
  if (!s_.keystore.initialized()) return "SolPi";
  return "SolPi-" + s_.keystore.address().substr(0, 4);
}

void Controller::showIdle() {
  mode_ = Mode::Idle;
  const auto& ks = s_.keystore;
  switch (ks.status()) {
    case storage::Keystore::Status::Empty:
      base_ = {"SOLPI WALLET", "RUN SETUP IN APP", ui::colors::kIdle};
      break;
    case storage::Keystore::Status::Error:
      base_ = {"KEYSTORE ERROR", "SEE DEVICE LOGS", ui::colors::kHigh};
      break;
    default:
      base_ = {deviceName(), abbreviateAddress(ks.address()) + (ks.ready() ? "" : " LOCK"),
               ui::colors::kIdle};
      break;
  }
}

void Controller::showNotice(const std::string& top, const std::string& bottom, ui::Rgb color,
                            TimeMs durationMs) {
  mode_ = Mode::Notice;
  base_ = {top, bottom, color};
  noticeUntil_ = s_.clock.nowMs() + durationMs;
}

std::string Controller::holdBar() const {
  const TimeMs held = s_.clock.nowMs() - s_.buttons.pressedAt(ui::Button::Right);
  const TimeMs total = s_.buttons.holdMs();
  const int filled = static_cast<int>(std::min<TimeMs>(10, held * 10 / std::max<TimeMs>(1, total)));
  return "HOLD " + std::string(static_cast<size_t>(filled), '#') +
         std::string(static_cast<size_t>(10 - filled), '-');
}

bool Controller::holdStartedInSession(const ui::ButtonEvent& ev) const {
  return ev.pressedAt >= sessionStart_;
}

void Controller::render() {
  ui::Screen out = base_;
  const bool holdMode =
      mode_ == Mode::Review ||
      (mode_ == Mode::Words && wordIndex_ == static_cast<int>(crypto::kMnemonicWords) - 1);
  if (!hint_.empty()) {
    out.bottom = hint_;
  } else if (holdMode && s_.buttons.pressed(ui::Button::Right) &&
             s_.buttons.pressedAt(ui::Button::Right) >= sessionStart_ &&
             s_.clock.nowMs() - s_.buttons.pressedAt(ui::Button::Right) >= 150) {
    out.bottom = holdBar();
  }
  s_.screen.show(out);
}

}  // namespace wfw::device
