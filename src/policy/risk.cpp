#include "policy/risk.h"

#include <map>

#include "solana/format.h"

namespace wfw::policy {

std::optional<RiskLevel> parseRiskLevel(const std::string& text) {
  if (text == "low") return RiskLevel::Low;
  if (text == "medium") return RiskLevel::Medium;
  if (text == "high") return RiskLevel::High;
  return std::nullopt;
}

const char* toString(RiskLevel level) {
  switch (level) {
    case RiskLevel::Low: return "low";
    case RiskLevel::Medium: return "medium";
    case RiskLevel::High: return "high";
  }
  return "high";
}

const char* toLabel(RiskLevel level) {
  switch (level) {
    case RiskLevel::Low: return "LOW";
    case RiskLevel::Medium: return "MEDIUM";
    case RiskLevel::High: return "HIGH";
  }
  return "HIGH";
}

RiskResult assess(const solana::TxSummary& tx, RiskLevel appLevel, const std::string& appReason,
                  const std::function<bool(const PubKey&)>& isKnownRecipient, const PubKey& self,
                  const RiskPolicy& policy) {
  RiskResult r;
  r.appLevel = appLevel;

  bool unknownTx = !tx.fullyRecognised();
  bool newOverLimit = false;
  bool newRecipient = false;
  bool highFee = tx.maxPriorityFeeLamports > policy.maxPriorityFeeLamports;

  // Sum per recipient so a large send cannot be split into small transfers.
  std::map<PubKey, uint64_t> perRecipient;
  for (const auto& t : tx.transfers) {
    if (t.to == self) continue;
    uint64_t& total = perRecipient[t.to];
    total = (UINT64_MAX - total < t.lamports) ? UINT64_MAX : total + t.lamports;
  }
  for (const auto& [to, total] : perRecipient) {
    if (isKnownRecipient(to)) continue;
    newRecipient = true;
    r.newRecipients.push_back(to);
    if (total > policy.newRecipientLimitLamports) newOverLimit = true;
  }

  if (unknownTx) r.flags.push_back("UNKNOWN TX");
  if (newOverLimit) r.flags.push_back("NEW+OVER LIMIT");
  if (highFee) r.flags.push_back("HIGH FEE");
  if (newRecipient) r.flags.push_back("NEW RECIPIENT");

  if (unknownTx || newOverLimit || highFee) {
    r.deviceLevel = RiskLevel::High;
  } else if (newRecipient) {
    r.deviceLevel = RiskLevel::Medium;
  }
  r.level = maxLevel(r.deviceLevel, appLevel);

  // A device finding is always shown: app-supplied text must never be able to
  // paper over it (e.g. "KNOWN RECIPIENT" on a never-seen address).
  if (!r.flags.empty()) {
    r.reason = r.flags.front();
  } else if (!appReason.empty()) {
    r.reason = lcdSanitize(appReason).substr(0, kLcdCols);
  } else if (appLevel > RiskLevel::Low) {
    r.reason = std::string("APP: ") + toLabel(appLevel);
  } else {
    r.reason = perRecipient.empty() ? "NO TRANSFER" : "KNOWN RECIPIENT";
  }
  return r;
}

}  // namespace wfw::policy
