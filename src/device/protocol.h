#pragma once

// Serial protocol constants. The full contract (request/response shapes,
// timing, error semantics) is in docs/firmware/protocol-contract.md.

namespace wfw::device {

constexpr int kProtocolVersion = 1;
constexpr size_t kMaxLineBytes = 8192;  // a 1232-byte message is ~1.7 KB of base64

namespace cmd {
constexpr const char* kPing = "ping";
constexpr const char* kGetPubkey = "get_pubkey";
constexpr const char* kSetup = "setup";
constexpr const char* kSignTx = "sign_tx";
// Extensions beyond the four commands in build manual Step 4:
constexpr const char* kShowWords = "show_words";          // re-show words until backup confirmed
constexpr const char* kConfirmBackup = "confirm_backup";  // check the typed-back phrase
constexpr const char* kCancel = "cancel";                 // abort the pending device prompt
constexpr const char* kHandshake = "handshake";           // Step 12 encrypted channel
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
constexpr const char* kRejected = "rejected";
constexpr const char* kCancelled = "cancelled";
constexpr const char* kTimeout = "timeout";
constexpr const char* kPhraseMismatch = "phrase_mismatch";
constexpr const char* kBackupAlreadyConfirmed = "backup_already_confirmed";
constexpr const char* kPinLocked = "pin_locked";
constexpr const char* kSecureChannelRequired = "secure_channel_required";
constexpr const char* kNoSecureChannel = "no_secure_channel";
constexpr const char* kDecryptFailed = "decrypt_failed";
constexpr const char* kHandshakeFailed = "handshake_failed";
constexpr const char* kInternal = "internal_error";
}  // namespace err

}  // namespace wfw::device
