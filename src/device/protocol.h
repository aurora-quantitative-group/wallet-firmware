#pragma once

// Aurora serial protocol constants. The full contract (request/response
// shapes, timing, error semantics) is in docs/firmware/protocol-contract.md;
// the Aurora desktop app (wallet-software) implements the other side.

namespace wfw::device {

constexpr int kProtocolVersion = 2;
constexpr size_t kMaxLineBytes = 8192;  // a 1232-byte message is ~1.7 KB of base64

namespace cmd {
constexpr const char* kPing = "ping";              // signed identity + wallet manifest
constexpr const char* kSetup = "setup";            // new wallet: words + on-device check
constexpr const char* kShowSeed = "show_seed";     // finish an unconfirmed backup
constexpr const char* kPair = "pair";              // approve this app on the device
constexpr const char* kSignTx = "sign_tx";         // clear-sign a Solana message
constexpr const char* kCancel = "cancel";          // abort the pending device prompt
constexpr const char* kReset = "reset";            // erase the wallet (confirmed on the device)
constexpr const char* kBalances = "balances";      // balances for the device's wallet list
constexpr const char* kGetPubkey = "get_pubkey";   // unsigned convenience lookup
constexpr const char* kHandshake = "handshake";    // optional encrypted channel
}  // namespace cmd

namespace err {
constexpr const char* kInvalidJson = "invalid_json";
constexpr const char* kInvalidRequest = "invalid_request";
constexpr const char* kUnknownCmd = "unknown_cmd";
constexpr const char* kLineTooLong = "line_too_long";
constexpr const char* kBusy = "busy";
constexpr const char* kNotInitialized = "not_initialized";
constexpr const char* kAlreadyInitialized = "already_initialized";
constexpr const char* kStorageError = "storage_error";
constexpr const char* kDisplayUnavailable = "display_unavailable";
constexpr const char* kInvalidMessage = "invalid_message";
constexpr const char* kNotSigner = "not_signer";
constexpr const char* kWrongWallet = "wrong_wallet";
constexpr const char* kRejected = "rejected";
constexpr const char* kCancelled = "cancelled";
constexpr const char* kTimeout = "timeout";
constexpr const char* kBackupAlreadyConfirmed = "backup_already_confirmed";
constexpr const char* kPinLocked = "pin_locked";
constexpr const char* kSecureChannelRequired = "secure_channel_required";
constexpr const char* kNoSecureChannel = "no_secure_channel";
constexpr const char* kDecryptFailed = "decrypt_failed";
constexpr const char* kHandshakeFailed = "handshake_failed";
constexpr const char* kAppDisconnected = "app_disconnected";
constexpr const char* kInternal = "internal_error";
}  // namespace err

}  // namespace wfw::device
