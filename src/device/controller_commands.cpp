// Immediate commands (ping, get_pubkey, cancel, handshake), pairing, and the
// user-facing text for every error code.
#include <algorithm>
#include <map>

#include "device/controller.h"
#include "device/protocol.h"
#include "util/log.h"

namespace wfw::device {

namespace {

const char* stateName(storage::Keystore::Status st) {
  switch (st) {
    case storage::Keystore::Status::Empty: return "uninitialized";
    case storage::Keystore::Status::Locked: return "locked";
    case storage::Keystore::Status::Ready: return "ready";
    case storage::Keystore::Status::Error: return "error";
  }
  return "error";
}

bool validChallenge(const nlohmann::json& v) {
  return v.is_string() && v.get_ref<const std::string&>().size() >= 8 &&
         v.get_ref<const std::string&>().size() <= 128;
}

}  // namespace

std::string Controller::humanError(const std::string& code) {
  static const std::map<std::string, std::string> kText = {
      {err::kInvalidJson, "Aurora received a malformed request."},
      {err::kInvalidRequest, "Aurora received an invalid request."},
      {err::kUnknownCmd, "Aurora does not support this request. Update the firmware or the app."},
      {err::kLineTooLong, "The request was too large for Aurora."},
      {err::kBusy, "Aurora is waiting for you on its screen. Finish or cancel that first."},
      {err::kNotInitialized, "Aurora has no wallet yet. Set it up first."},
      {err::kAlreadyInitialized, "Aurora already has a wallet. It is never replaced."},
      {err::kStorageError, "Aurora cannot read or write its wallet storage."},
      {err::kDisplayUnavailable, "Aurora's screen is not responding, so nothing can be confirmed."},
      {err::kInvalidMessage, "Aurora could not read this transaction."},
      {err::kNotSigner, "This transaction does not need Aurora's signature."},
      {err::kWrongWallet, "That wallet is not on this Aurora."},
      {err::kRejected, "Rejected on Aurora."},
      {err::kCancelled, "Cancelled."},
      {err::kTimeout, "No answer on Aurora in time."},
      {err::kBackupAlreadyConfirmed,
       "Your backup is confirmed. For your security, Aurora never shows the recovery phrase "
       "again."},
      {err::kPinLocked, "Too many wrong PINs. Aurora stays locked; recover with your 12 words."},
      {err::kSecureChannelRequired, "Aurora requires an encrypted session first."},
      {err::kNoSecureChannel, "No encrypted session with Aurora."},
      {err::kDecryptFailed, "Aurora could not decrypt the request."},
      {err::kHandshakeFailed, "Aurora could not start an encrypted session."},
      {err::kLocked, "Aurora is locked. Enter your password to unlock it."},
      {err::kWrongPassword, "Wrong password."},
      {err::kAppDisconnected,
       "Aurora is disconnected from apps. On Aurora, open the menu and choose CONNECT APP."},
  };
  const auto it = kText.find(code);
  return it == kText.end() ? "Aurora reported an internal error." : it->second;
}

nlohmann::json Controller::walletManifest() const {
  json wallets = json::array();
  const auto& ks = s_.keystore;
  if (!ks.initialized()) return wallets;
  const std::string sol = ks.address();
  // The same SLIP-0010 account on both clusters; only devnet can sign.
  wallets.push_back({{"id", "sol-0"}, {"name", "Solana wallet"}, {"chain", "SOL"},
                     {"network", "mainnet"}, {"account", 0}, {"address", sol}});
  wallets.push_back({{"id", "sol-dev"}, {"name", "Devnet wallet"}, {"chain", "SOL"},
                     {"network", "devnet"}, {"account", 0}, {"address", sol}});
  if (!ks.bitcoinAddress().empty()) {
    wallets.push_back({{"id", "btc-0"}, {"name", "Bitcoin wallet"}, {"chain", "BTC"},
                       {"network", "mainnet"}, {"account", 0}, {"address", ks.bitcoinAddress()}});
  }
  return wallets;
}

// The app verifies the identity signature over these exact payload bytes.
nlohmann::json Controller::signedPayload(const json& payload) const {
  const std::string text = payload.dump(-1, ' ', false, json::error_handler_t::replace);
  return {{"ok", true}, {"payload", text}, {"signature", s_.identity.sign(text)}};
}

void Controller::handlePing(const Ctx& ctx, const json& params) {
  const auto challenge = params.find("challenge");
  if (challenge == params.end() || !validChallenge(*challenge)) {
    replyError(ctx, err::kInvalidRequest, "challenge must be a string of 8-128 characters");
    return;
  }
  const auto& ks = s_.keystore;
  reply(ctx, signedPayload({{"challenge", *challenge},
                            {"device_id", s_.identity.deviceId()},
                            {"name", deviceName()},
                            {"has_seed", ks.initialized()},
                            {"wallets", walletManifest()},
                            {"firmware", WFW_VERSION},
                            {"protocol", kProtocolVersion},
                            {"state", stateName(ks.status())},
                            {"backup_confirmed", ks.backupConfirmed()},
                            {"pin_protected", ks.pinProtected()},
                            {"busy", sessionActive()},
                            {"display_ok", s_.screen.healthy()},
                            {"secure_channel", channel_.active()}}));
}

void Controller::handleGetPubkey(const Ctx& ctx) {
  const auto& ks = s_.keystore;
  if (ks.status() == storage::Keystore::Status::Error) {
    replyError(ctx, err::kStorageError, ks.lastError());
    return;
  }
  if (!ks.initialized()) {
    replyError(ctx, err::kNotInitialized, "run setup");
    return;
  }
  reply(ctx, {{"ok", true},
              {"pubkey", ks.address()},
              {"device_id", s_.identity.deviceId()},
              {"name", deviceName()},
              {"backup_confirmed", ks.backupConfirmed()},
              {"pin_protected", ks.pinProtected()},
              {"locked", !ks.ready()}});
}

void Controller::handleCancel(const Ctx& ctx) {
  const bool had = sessionActive();
  if (had) {
    finishPendingError(err::kCancelled, "cancelled by the app");
    clearSecrets();
    showNotice("CANCELLED", "BY THE APP", ui::colors::kIdle);
  }
  reply(ctx, {{"ok", true}, {"cancelled", had}});
}

// --------------------------------------------------------------- pairing --

void Controller::handlePair(const Ctx& ctx, const json& params) {
  if (!checkReadyForPrompt(ctx)) return;
  const auto& ks = s_.keystore;
  if (!ks.initialized()) {
    replyError(ctx, ks.status() == storage::Keystore::Status::Error ? err::kStorageError
                                                                    : err::kNotInitialized);
    return;
  }
  const auto challenge = params.find("challenge");
  const auto code = params.find("code");
  const bool codeOk = code != params.end() && code->is_string() &&
                      code->get_ref<const std::string&>().size() >= 4 &&
                      code->get_ref<const std::string&>().size() <= 6 &&
                      std::all_of(code->get_ref<const std::string&>().begin(),
                                  code->get_ref<const std::string&>().end(), ::isdigit);
  if (challenge == params.end() || !validChallenge(*challenge) || !codeOk) {
    replyError(ctx, err::kInvalidRequest, "pair needs a challenge and a 4-6 digit code");
    return;
  }
  pairChallenge_ = challenge->get<std::string>();
  pairCode_ = code->get<std::string>();
  beginPending(Pending::Pair, ctx, s_.config.reviewTimeoutS);
  requireUnlock([this] {
    mode_ = Mode::Pair;
    sessionStart_ = s_.clock.nowMs();
    base_ = {"PAIR CODE " + pairCode_, "HOLD R  L:REJECT", ui::colors::kSetup};
    s_.buzzer.beep();
  });
}

void Controller::onPairButton(const ui::ButtonEvent& ev) {
  if (ev.kind == ui::ButtonEvent::Kind::Press && ev.button == ui::Button::Left) {
    LOG_INFO("pairing rejected on device");
    finishPendingError(err::kRejected, "pairing rejected on the device");
    showNotice("PAIRING", "REJECTED", ui::colors::kIdle);
  } else if (ev.kind == ui::ButtonEvent::Kind::Hold && ev.button == ui::Button::Right &&
             holdStartedInSession(ev) && s_.screen.healthy()) {
    LOG_INFO("pairing approved on device");
    finishPending(signedPayload({{"challenge", pairChallenge_},
                                 {"code", pairCode_},
                                 {"device_id", s_.identity.deviceId()},
                                 {"paired", true}}));
    showNotice("PAIRED", "WITH THE APP", ui::colors::kLow, 2000);
    s_.buzzer.beep();
  }
}

// --------------------------------------------------------------- password --
// A wallet created with an app password is encrypted at rest (Argon2id +
// secretbox) and locked after a restart or auto_lock_s idle; the app sends
// the password to unlock it. Wrong attempts are counted across restarts.

void Controller::handleUnlock(const Ctx& ctx, const json& params) {
  auto& ks = s_.keystore;
  if (!ks.initialized()) {
    replyError(ctx, err::kNotInitialized, "there is no wallet to unlock");
    return;
  }
  if (ks.ready()) {
    reply(ctx, {{"ok", true}, {"state", "ready"}});
    return;
  }
  if (sessionActive()) {
    replyError(ctx, err::kBusy, "a device prompt is in progress");
    return;
  }
  if (pinFailures_ >= s_.config.pinMaxAttempts) {
    replyError(ctx, err::kPinLocked, "too many wrong passwords");
    return;
  }
  const auto pw = params.find("password");
  if (pw == params.end() || !pw->is_string() || pw->get_ref<const std::string&>().empty() ||
      pw->get_ref<const std::string&>().size() > 128) {
    replyError(ctx, err::kInvalidRequest, "password must be 1-128 characters");
    return;
  }
  base_ = {"UNLOCKING", "PLEASE WAIT", ui::colors::kSetup};
  render();  // Argon2id takes about a second on a Pi 4
  std::string password = pw->get<std::string>();
  std::string why;
  const bool ok = ks.unlock(password, why);
  sodium_memzero(password.data(), password.size());
  if (ok) {
    pinFailures_ = 0;
    ks.setPinFailures(0, why);
    lastActivity_ = s_.clock.nowMs();
    LOG_INFO("unlocked by password");
    showIdle();
    reply(ctx, {{"ok", true}, {"state", "ready"}});
    return;
  }
  if (why != "wrong PIN") {
    showIdle();
    replyError(ctx, err::kInternal, why);
    return;
  }
  ++pinFailures_;
  ks.setPinFailures(pinFailures_, why);
  LOG_WARN("wrong password (%d of %d)", pinFailures_, s_.config.pinMaxAttempts);
  showNotice("WRONG PASSWORD", "", ui::colors::kHigh, 2000);
  const int left = s_.config.pinMaxAttempts - pinFailures_;
  if (left <= 0) {
    replyError(ctx, err::kPinLocked, "too many wrong passwords");
    return;
  }
  reply(ctx, {{"ok", false},
              {"error", err::kWrongPassword},
              {"message", std::to_string(left) + " attempts left"},
              {"attempts_left", left}});
}

void Controller::handleLock(const Ctx& ctx) {
  if (sessionActive()) {
    finishPendingError(err::kCancelled, "Aurora was locked");
    clearSecrets();
  }
  s_.keystore.lock();
  showIdle();
  reply(ctx, {{"ok", true}, {"locked", s_.keystore.pinProtected()}});
}

// ------------------------------------------------------ encrypted channel --

void Controller::handleHandshake(const Ctx& ctx, const json& params) {
  const Ctx plain{ctx.id, false};  // the handshake itself is never encrypted
  if (sessionActive()) {
    replyError(plain, err::kBusy, "a device prompt is in progress");
    return;
  }
  const auto key = params.find("client_pubkey");
  std::optional<Bytes> raw;
  if (key != params.end() && key->is_string()) raw = base64Decode(key->get<std::string>());
  if (!raw || raw->size() != 32) {
    replyError(plain, err::kInvalidRequest, "client_pubkey must be 32 bytes of base64");
    return;
  }
  std::array<uint8_t, 32> clientKey{};
  std::copy(raw->begin(), raw->end(), clientKey.begin());
  std::array<uint8_t, 32> deviceEph{};
  std::array<uint8_t, 64> sig{};
  std::string why;
  // Signed by the device identity, so no wallet or PIN unlock is needed.
  if (!channel_.accept(clientKey, s_.identity.keyPair(), deviceEph, sig, why)) {
    replyError(plain, err::kHandshakeFailed, why);
    return;
  }
  LOG_INFO("secure channel established");
  reply(plain, {{"ok", true},
                {"device_ephemeral", base64Encode(deviceEph.data(), deviceEph.size())},
                {"device_id", s_.identity.deviceId()},
                {"signature", base64Encode(sig.data(), sig.size())}});
}

}  // namespace wfw::device
