#include "solana/analyze.h"
#include "solana/message.h"
#include "test.h"
#include "test_vectors.h"

using namespace wfw;
using namespace wfw::solana;

namespace {

Bytes hex(const char* s) { return *hexDecode(s); }
PubKey key(const char* b58) { return *decodeAddress(b58); }
const PubKey kSelf = key(test::kAbandonAddress);
const PubKey kTo = key(test::kRecipient);

bool parse(const Bytes& b, Message& m) {
  std::string err;
  return parseMessage(b.data(), b.size(), m, err);
}

}  // namespace

TEST(compact_u16_vectors) {
  struct V {
    Bytes in;
    bool ok;
    uint16_t value;
  };
  const V cases[] = {
      {{0x00}, true, 0},
      {{0x7f}, true, 127},
      {{0x80, 0x01}, true, 128},
      {{0xff, 0x7f}, true, 16383},
      {{0x80, 0x80, 0x01}, true, 16384},
      {{0xff, 0xff, 0x03}, true, 65535},
      {{0x80, 0x00}, false, 0},        // non-canonical
      {{0xff, 0xff, 0x04}, false, 0},  // overflow
      {{0x80}, false, 0},              // truncated
      {{}, false, 0},
  };
  for (const auto& c : cases) {
    size_t pos = 0;
    uint16_t v = 0;
    const bool ok = readCompactU16(c.in.data(), c.in.size(), pos, v);
    CHECK_EQ(ok, c.ok);
    if (ok) CHECK_EQ(v, c.value);
  }
}

TEST(parse_web3js_transfer) {
  Message m;
  REQUIRE(parse(hex(test::kTransferMsgHex), m));
  CHECK(!m.versioned);
  CHECK_EQ(m.numRequiredSignatures, uint8_t{1});
  CHECK_EQ(m.accountKeys.size(), size_t{3});
  CHECK(m.accountKeys[0] == kSelf);
  CHECK(m.isWritable(0) && m.isWritable(1) && !m.isWritable(2));
  CHECK_EQ(base58Encode(m.recentBlockhash), std::string("EETubP5AKHgjPAhzPAFcb8BAY1hMH639CWCFTqi3hq1k"));
  const TxSummary s = analyze(m, kSelf);
  CHECK(s.fullyRecognised());
  REQUIRE(s.transfers.size() == 1);
  CHECK(s.transfers[0].to == kTo);
  CHECK_EQ(s.transfers[0].lamports, uint64_t{500000000});
  CHECK_EQ(s.maxPriorityFeeLamports, uint64_t{0});
}

TEST(parse_solana_pay_reference_transfer) {
  Message m;
  REQUIRE(parse(hex(test::kSolanaPayMsgHex), m));
  const TxSummary s = analyze(m, kSelf);
  CHECK(s.fullyRecognised());
  REQUIRE(s.transfers.size() == 1);
  CHECK(s.transfers[0].to == kTo);
  CHECK_EQ(s.transfers[0].lamports, uint64_t{1234567});
  REQUIRE(m.instructions.size() == 1);
  CHECK_EQ(m.instructions[0].accounts.size(), size_t{3});
  CHECK(m.accountKeys[m.instructions[0].accounts[2]] == key(test::kSolanaPayRef));
}

TEST(parse_compute_budget_and_memo) {
  Message m;
  REQUIRE(parse(hex(test::kBudgetMemoMsgHex), m));
  const TxSummary s = analyze(m, kSelf);
  CHECK(s.fullyRecognised());
  REQUIRE(s.computeUnitLimit.has_value());
  CHECK_EQ(*s.computeUnitLimit, 300000u);
  REQUIRE(s.computeUnitPriceMicroLamports.has_value());
  CHECK_EQ(*s.computeUnitPriceMicroLamports, uint64_t{50000});
  CHECK_EQ(s.maxPriorityFeeLamports, uint64_t{15000});  // 50000 * 300000 / 1e6
  REQUIRE(s.memos.size() == 1);
  CHECK_EQ(s.memos[0], std::string("coffee for alice"));
  REQUIRE(s.transfers.size() == 1);
  CHECK_EQ(s.transfers[0].lamports, uint64_t{42});
}

TEST(parse_v0_message) {
  Message m;
  REQUIRE(parse(hex(test::kV0MsgHex), m));
  CHECK(m.versioned);
  CHECK_EQ(m.version, uint8_t{0});
  CHECK_EQ(m.lookupTableCount, size_t{0});
  const TxSummary s = analyze(m, kSelf);
  CHECK(s.fullyRecognised());
  REQUIRE(s.transfers.size() == 1);
  CHECK_EQ(s.transfers[0].lamports, uint64_t{2000000000});
}

TEST(unknown_program_is_flagged) {
  Message m;
  REQUIRE(parse(hex(test::kUnknownMsgHex), m));
  const TxSummary s = analyze(m, kSelf);
  CHECK(!s.fullyRecognised());
  REQUIRE(s.unknown.size() == 1);
  CHECK_EQ(s.unknown[0].reason, std::string("UNKNOWN PROGRAM"));
  REQUIRE(s.unknown[0].program.has_value());
  CHECK_EQ(base58Encode(*s.unknown[0].program),
           std::string("TokenkegQfeZyiNwAJbNbGKPFXCWuBvf9Ss623VQ5DA"));
}

TEST(transfer_from_other_account_is_foreign) {
  Message m;
  REQUIRE(parse(hex(test::kTransferMsgHex), m));
  const TxSummary s = analyze(m, kTo);  // pretend the device is the recipient
  CHECK(s.transfers.empty());
  REQUIRE(s.unknown.size() == 1);
  CHECK_EQ(s.unknown[0].reason, std::string("FOREIGN SOURCE"));
}

TEST(other_system_instruction_and_bad_transfer) {
  Bytes b = hex(test::kTransferMsgHex);
  // The instruction data starts 12 bytes before the end: u32 LE discriminator.
  Bytes other = b;
  other[other.size() - 12] = 0;  // CreateAccount
  Message m;
  REQUIRE(parse(other, m));
  TxSummary s = analyze(m, kSelf);
  REQUIRE(s.unknown.size() == 1);
  CHECK_EQ(s.unknown[0].reason, std::string("OTHER SYSTEM IX"));
}

TEST(duplicate_compute_budget_is_flagged) {
  // Same price instruction twice: [tag 3, u64 price].
  Message m;
  REQUIRE(parse(hex(test::kBudgetMemoMsgHex), m));
  m.instructions.insert(m.instructions.begin(), m.instructions[1]);
  const TxSummary s = analyze(m, kSelf);
  REQUIRE(s.unknown.size() == 1);
  CHECK_EQ(s.unknown[0].reason, std::string("DUP BUDGET IX"));
}

TEST(priority_fee_without_limit_uses_default_units) {
  Message m;
  REQUIRE(parse(hex(test::kBudgetMemoMsgHex), m));
  m.instructions.erase(m.instructions.begin());  // drop SetComputeUnitLimit
  const TxSummary s = analyze(m, kSelf);
  // 2 non-budget instructions * 200k CU * 50000 micro-lamports / 1e6
  CHECK_EQ(s.maxPriorityFeeLamports, uint64_t{20000});
}

TEST(rejects_malformed_messages) {
  const Bytes good = hex(test::kTransferMsgHex);
  Message m;
  Bytes b = good;
  b.pop_back();
  CHECK(!parse(b, m));  // truncated
  b = good;
  b.push_back(0);
  CHECK(!parse(b, m));  // trailing byte
  b = good;
  b[0] = 0;
  CHECK(!parse(b, m));  // no signers
  b = good;
  b[1] = 1;
  CHECK(!parse(b, m));  // fee payer read-only
  b = good;
  b[2] = 3;
  CHECK(!parse(b, m));  // more read-only unsigned than keys allow
  b = good;
  std::copy(b.begin() + 4, b.begin() + 36, b.begin() + 36);
  CHECK(!parse(b, m));  // duplicate account keys
  b = good;
  b[3 + 1 + 3 * 32 + 32] = 0x7f;  // 127 instructions claimed, 15 bytes left
  std::string err;
  CHECK(!parseMessage(b.data(), b.size(), m, err));
  CHECK_EQ(err, std::string("bad instruction count"));
  b = good;
  b[0] = 0x81;
  CHECK(!parse(b, m));  // v1 message
  CHECK(!parse(Bytes{}, m));
  CHECK(!parse(Bytes(kMaxMessageBytes + 1, 1), m));
  // Program index 0 (fee payer) and out-of-range account index.
  b = good;
  const size_t ixStart = 3 + 1 + 3 * 32 + 32 + 1;  // header, key count, keys, hash, ix count
  b[ixStart] = 0;
  CHECK(!parse(b, m));
  b = good;
  b[ixStart + 2] = 9;
  CHECK(!parse(b, m));
}
