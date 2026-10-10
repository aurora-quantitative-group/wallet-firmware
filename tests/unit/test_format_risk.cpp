#include <set>

#include "policy/risk.h"
#include "solana/format.h"
#include "test.h"

using namespace wfw;
using namespace wfw::policy;

TEST(format_sol_amounts) {
  CHECK_EQ(formatSol(500000000), std::string("0.50"));
  CHECK_EQ(formatSol(1000000000), std::string("1.00"));
  CHECK_EQ(formatSol(0), std::string("0.00"));
  CHECK_EQ(formatSol(5000), std::string("0.000005"));
  CHECK_EQ(formatSol(1), std::string("0.000000001"));
  CHECK_EQ(formatSol(1234567), std::string("0.001234567"));
  CHECK_EQ(formatSol(12345000000000), std::string("12345.00"));
  CHECK_EQ(formatSol(UINT64_MAX), std::string("18446744073.709551615"));
}

TEST(lcd_text_helpers) {
  CHECK_EQ(abbreviateAddress("7xKXtg2CW87d97TXJSDpbD5jBkheTqA83TZRuJosgAsU"),
           std::string("7xKX..gAsU"));
  CHECK_EQ(abbreviateAddress("short"), std::string("short"));
  CHECK_EQ(lcdLine("abc"), std::string("abc             "));
  CHECK_EQ(lcdLine("0123456789abcdefXYZ"), std::string("0123456789abcdef"));
  CHECK_EQ(lcdSanitize("a\\b~c\x01\xc3\xa9"), std::string("a?b?c???"));
}

namespace {

PubKey keyOf(uint8_t b) {
  PubKey k{};
  k.fill(b);
  return k;
}

const PubKey kSelf = keyOf(1);
const PubKey kKnown = keyOf(2);
const PubKey kNew = keyOf(3);

solana::TxSummary sendTo(const PubKey& to, uint64_t lamports) {
  solana::TxSummary s;
  s.feePayer = kSelf;
  s.transfers.push_back({kSelf, to, lamports});
  return s;
}

RiskResult run(const solana::TxSummary& tx, RiskLevel app, const std::string& reason = "") {
  const std::set<PubKey> known = {kKnown};
  RiskPolicy policy;  // limit 1 SOL, fee cap 0.01 SOL
  return assess(tx, app, reason, [&](const PubKey& k) { return known.count(k) != 0; }, kSelf,
                policy);
}

}  // namespace

TEST(risk_known_recipient_follows_app) {
  RiskResult r = run(sendTo(kKnown, 5 * kLamportsPerSol), RiskLevel::Low);
  CHECK(r.level == RiskLevel::Low);
  CHECK_EQ(r.reason, std::string("KNOWN RECIPIENT"));
  r = run(sendTo(kKnown, 1), RiskLevel::High, "Drains balance");
  CHECK(r.level == RiskLevel::High);
  CHECK(r.deviceLevel == RiskLevel::Low);
  CHECK_EQ(r.reason, std::string("Drains balance"));
  r = run(sendTo(kKnown, 1), RiskLevel::Medium);
  CHECK_EQ(r.reason, std::string("APP: MEDIUM"));
}

TEST(risk_new_recipient_is_at_least_medium) {
  const RiskResult r = run(sendTo(kNew, kLamportsPerSol), RiskLevel::Low);  // == limit
  CHECK(r.level == RiskLevel::Medium);
  CHECK_EQ(r.reason, std::string("NEW RECIPIENT"));
  REQUIRE(r.newRecipients.size() == 1);
  CHECK(r.newRecipients[0] == kNew);
}

TEST(risk_new_recipient_over_limit_is_high_whatever_the_app_says) {
  const RiskResult r = run(sendTo(kNew, kLamportsPerSol + 1), RiskLevel::Low);
  CHECK(r.level == RiskLevel::High);
  CHECK(r.appLevel == RiskLevel::Low);
  CHECK_EQ(r.reason, std::string("NEW+OVER LIMIT"));
}

TEST(risk_split_transfers_are_summed) {
  solana::TxSummary tx = sendTo(kNew, kLamportsPerSol / 2 + 1);
  tx.transfers.push_back({kSelf, kNew, kLamportsPerSol / 2});
  CHECK(run(tx, RiskLevel::Low).level == RiskLevel::High);
}

TEST(risk_unknown_instruction_and_fee) {
  solana::TxSummary tx = sendTo(kKnown, 1);
  tx.unknown.push_back({std::nullopt, "UNKNOWN PROGRAM"});
  RiskResult r = run(tx, RiskLevel::Low);
  CHECK(r.level == RiskLevel::High);
  CHECK_EQ(r.reason, std::string("UNKNOWN TX"));

  tx = sendTo(kKnown, 1);
  tx.maxPriorityFeeLamports = 10000001;
  r = run(tx, RiskLevel::Low);
  CHECK(r.level == RiskLevel::High);
  CHECK_EQ(r.reason, std::string("HIGH FEE"));
}

TEST(risk_self_transfer_and_empty) {
  CHECK(run(sendTo(kSelf, 100 * kLamportsPerSol), RiskLevel::Low).level == RiskLevel::Low);
  solana::TxSummary empty;
  const RiskResult r = run(empty, RiskLevel::Low);
  CHECK(r.level == RiskLevel::Low);
  CHECK_EQ(r.reason, std::string("NO TRANSFER"));
}

TEST(risk_level_parsing) {
  CHECK(parseRiskLevel("low") == RiskLevel::Low);
  CHECK(parseRiskLevel("medium") == RiskLevel::Medium);
  CHECK(parseRiskLevel("high") == RiskLevel::High);
  CHECK(!parseRiskLevel("HIGH").has_value());
  CHECK(!parseRiskLevel("").has_value());
}
