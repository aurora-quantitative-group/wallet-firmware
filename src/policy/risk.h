#pragma once

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "solana/analyze.h"

namespace wfw::policy {

enum class RiskLevel : int { Low = 0, Medium = 1, High = 2 };

std::optional<RiskLevel> parseRiskLevel(const std::string& text);  // "low"|"medium"|"high"
const char* toString(RiskLevel level);                              // "low"...
const char* toLabel(RiskLevel level);                               // "LOW"...
inline RiskLevel maxLevel(RiskLevel a, RiskLevel b) { return a > b ? a : b; }

struct RiskPolicy {
  // A first-time recipient receiving more than this is high risk.
  uint64_t newRecipientLimitLamports = 1000000000ULL;  // 1 SOL
  // A priority fee that could exceed this is high risk.
  uint64_t maxPriorityFeeLamports = 10000000ULL;  // 0.01 SOL
};

struct RiskResult {
  RiskLevel level = RiskLevel::Low;        // what the device shows and enforces
  RiskLevel deviceLevel = RiskLevel::Low;  // from the device's own rules
  RiskLevel appLevel = RiskLevel::Low;     // as sent by the app
  std::string reason;                      // <= 16 chars, fits one LCD line
  std::vector<std::string> flags;          // every device rule that fired
  std::vector<PubKey> newRecipients;
};

// The device's own rule (build manual Step 7): the app can only raise the
// level, never lower it below what the device decides.
//  - unrecognised instruction                 -> high  "UNKNOWN TX"
//  - recipient never signed for, total > limit -> high  "NEW+OVER LIMIT"
//  - priority fee bound above the cap          -> high  "HIGH FEE"
//  - recipient never signed for                -> medium "NEW RECIPIENT"
RiskResult assess(const solana::TxSummary& tx, RiskLevel appLevel, const std::string& appReason,
                  const std::function<bool(const PubKey&)>& isKnownRecipient, const PubKey& self,
                  const RiskPolicy& policy);

}  // namespace wfw::policy
