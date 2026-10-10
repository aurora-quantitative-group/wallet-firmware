// Immediate commands: ping, get_pubkey, cancel, confirm_backup, handshake.
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

}  // namespace

void Controller::handlePing(const Ctx& ctx) {
  reply(ctx, {{"ok", true},
              {"pong", true},
              {"version", WFW_VERSION},
              {"protocol", kProtocolVersion},
              {"initialized", s_.keystore.initialized()},
              {"state", stateName(s_.keystore.status())},
              {"name", deviceName()},
              {"busy", sessionActive()},
              {"display_ok", s_.screen.healthy()},
              {"secure_channel", channel_.active()}});
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

void Controller::handleConfirmBackup(const Ctx& ctx, const json& req) {
  auto& ks = s_.keystore;
  if (ks.status() == storage::Keystore::Status::Error) {
    replyError(ctx, err::kStorageError, ks.lastError());
    return;
  }
  if (!ks.initialized()) {
    replyError(ctx, err::kNotInitialized, "run setup");
    return;
  }
  if (ks.backupConfirmed()) {
    replyError(ctx, err::kBackupAlreadyConfirmed);
    return;
  }
  const auto words = req.find("words");
  if (words == req.end() || !words->is_string()) {
    replyError(ctx, err::kInvalidRequest, "missing string field 'words'");
    return;
  }
  // Compare by derived public key, so no PIN unlock is needed.
  crypto::Mnemonic typed;
  bool match = typed.fromText(words->get<std::string>());
  if (match) {
    crypto::KeyPair kp;
    crypto::deriveSolanaKeyPair(typed, kp);
    match = kp.publicKey == ks.publicKey();
  }
  if (!match) {
    replyError(ctx, err::kPhraseMismatch, "the words do not match this device");
    return;
  }
  std::string why;
  if (!ks.setBackupConfirmed(why)) {
    replyError(ctx, err::kStorageError, why);
    return;
  }
  LOG_INFO("backup confirmed");
  reply(ctx, {{"ok", true}, {"backup_confirmed", true}});
}

void Controller::handleHandshake(const Ctx& ctx, const json& req) {
  const Ctx plain{ctx.id, false};  // the handshake itself is never encrypted
  if (sessionActive()) {
    replyError(plain, err::kBusy, "a device prompt is in progress");
    return;
  }
  const auto& ks = s_.keystore;
  if (!ks.initialized()) {
    replyError(plain, ks.status() == storage::Keystore::Status::Error ? err::kStorageError
                                                                      : err::kNotInitialized);
    return;
  }
  const auto key = req.find("client_pubkey");
  std::optional<Bytes> raw;
  if (key != req.end() && key->is_string()) raw = base64Decode(key->get<std::string>());
  if (!raw || raw->size() != 32) {
    replyError(plain, err::kInvalidRequest, "client_pubkey must be 32 bytes of base64");
    return;
  }
  std::copy(raw->begin(), raw->end(), handshakeClientKey_.begin());
  channel_.reset();
  if (ks.ready()) {
    pending_ = Pending::Handshake;
    pendingCtx_ = plain;
    completeHandshake();
    return;
  }
  if (!checkReadyForPrompt(plain)) return;
  beginPending(Pending::Handshake, plain, s_.config.reviewTimeoutS);
  requireUnlock([this] { completeHandshake(); });
}

void Controller::completeHandshake() {
  std::array<uint8_t, 32> deviceEph{};
  std::array<uint8_t, 64> sig{};
  std::string why;
  const crypto::KeyPair* kp = s_.keystore.keyPair();
  if (kp == nullptr || !channel_.accept(handshakeClientKey_, *kp, deviceEph, sig, why)) {
    finishPendingError(err::kHandshakeFailed, kp ? why : "keystore locked");
    showIdle();
    return;
  }
  LOG_INFO("secure channel established");
  finishPending({{"ok", true},
                 {"device_ephemeral", base64Encode(deviceEph.data(), deviceEph.size())},
                 {"pubkey", s_.keystore.address()},
                 {"signature", base64Encode(sig.data(), sig.size())}});
  showIdle();
}

}  // namespace wfw::device
