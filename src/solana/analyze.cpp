#include "solana/analyze.h"

#include <cstring>

namespace wfw::solana {
namespace {

PubKey keyFromBase58(const char* text) {
  auto key = decodeAddress(text);
  return key ? *key : PubKey{};
}

uint32_t readU32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | static_cast<uint32_t>(p[1]) << 8 |
         static_cast<uint32_t>(p[2]) << 16 | static_cast<uint32_t>(p[3]) << 24;
}

uint64_t readU64(const uint8_t* p) {
  uint64_t v = 0;
  for (int i = 0; i < 8; ++i) v |= static_cast<uint64_t>(p[i]) << (8 * i);
  return v;
}

constexpr uint32_t kSystemTransfer = 2;
constexpr uint64_t kDefaultUnitsPerInstruction = 200000;
constexpr uint64_t kMaxComputeUnits = 1400000;

void unknownIx(TxSummary& s, const PubKey& program, const char* reason) {
  s.unknown.push_back({program, reason});
}

void analyzeSystem(const Message& msg, const CompiledInstruction& ix, const PubKey& self,
                   TxSummary& s) {
  const PubKey& program = systemProgramId();
  if (ix.data.size() < 4) {
    unknownIx(s, program, "BAD SYSTEM IX");
    return;
  }
  if (readU32(ix.data.data()) != kSystemTransfer) {
    unknownIx(s, program, "OTHER SYSTEM IX");
    return;
  }
  if (ix.data.size() != 12 || ix.accounts.size() < 2) {
    unknownIx(s, program, "BAD TRANSFER");
    return;
  }
  const uint8_t fromIdx = ix.accounts[0];
  const uint8_t toIdx = ix.accounts[1];
  if (!msg.isStatic(fromIdx) || !msg.isStatic(toIdx)) {
    unknownIx(s, program, "LOOKUP ACCOUNT");
    return;
  }
  const PubKey& from = msg.accountKeys[fromIdx];
  if (from != self || !msg.isSigner(fromIdx) || !msg.isWritable(fromIdx) ||
      !msg.isWritable(toIdx)) {
    unknownIx(s, program, "FOREIGN SOURCE");
    return;
  }
  // Extra accounts after [from, to] are allowed: Solana Pay appends the
  // read-only reference key(s) here.
  s.transfers.push_back({from, msg.accountKeys[toIdx], readU64(ix.data.data() + 4)});
}

void analyzeComputeBudget(const CompiledInstruction& ix, TxSummary& s, bool& seenHeap,
                          bool& seenDataSize) {
  const PubKey& program = computeBudgetProgramId();
  if (ix.data.empty()) {
    unknownIx(s, program, "BAD BUDGET IX");
    return;
  }
  const uint8_t tag = ix.data[0];
  const size_t len = ix.data.size();
  bool duplicate = false;
  switch (tag) {
    case 1:  // RequestHeapFrame(u32)
      if (len != 5) break;
      duplicate = seenHeap;
      seenHeap = true;
      if (!duplicate) return;
      break;
    case 2:  // SetComputeUnitLimit(u32)
      if (len != 5) break;
      duplicate = s.computeUnitLimit.has_value();
      s.computeUnitLimit = readU32(ix.data.data() + 1);
      if (!duplicate) return;
      break;
    case 3:  // SetComputeUnitPrice(u64 micro-lamports per CU)
      if (len != 9) break;
      duplicate = s.computeUnitPriceMicroLamports.has_value();
      s.computeUnitPriceMicroLamports = readU64(ix.data.data() + 1);
      if (!duplicate) return;
      break;
    case 4:  // SetLoadedAccountsDataSizeLimit(u32)
      if (len != 5) break;
      duplicate = seenDataSize;
      seenDataSize = true;
      if (!duplicate) return;
      break;
    default:
      break;
  }
  // Solana rejects duplicate budget instructions; treat them as suspicious.
  unknownIx(s, program, duplicate ? "DUP BUDGET IX" : "BAD BUDGET IX");
}

}  // namespace

const PubKey& systemProgramId() {
  static const PubKey k{};  // all zeros
  return k;
}
const PubKey& computeBudgetProgramId() {
  static const PubKey k = keyFromBase58("ComputeBudget111111111111111111111111111111");
  return k;
}
const PubKey& memoProgramId() {
  static const PubKey k = keyFromBase58("MemoSq4gqABAXKb96qnH8TysNcWxMyWCqXgDLGmfcHr");
  return k;
}
const PubKey& memoV1ProgramId() {
  static const PubKey k = keyFromBase58("Memo1UhkJRfHyvLMcVucJwxXeuD728EqVDDwQDxFMNo");
  return k;
}

TxSummary analyze(const Message& msg, const PubKey& self) {
  TxSummary s;
  s.feePayer = msg.accountKeys.at(0);
  bool seenHeap = false;
  bool seenDataSize = false;
  size_t nonBudgetInstructions = 0;

  for (const auto& ix : msg.instructions) {
    const PubKey& program = msg.accountKeys.at(ix.programIdIndex);
    if (program == systemProgramId()) {
      ++nonBudgetInstructions;
      analyzeSystem(msg, ix, self, s);
    } else if (program == computeBudgetProgramId()) {
      analyzeComputeBudget(ix, s, seenHeap, seenDataSize);
    } else if (program == memoProgramId() || program == memoV1ProgramId()) {
      ++nonBudgetInstructions;
      s.memos.emplace_back(ix.data.begin(), ix.data.end());
    } else {
      ++nonBudgetInstructions;
      unknownIx(s, program, "UNKNOWN PROGRAM");
    }
  }

  if (s.computeUnitPriceMicroLamports && *s.computeUnitPriceMicroLamports > 0) {
    uint64_t units = 0;
    if (s.computeUnitLimit) {
      units = *s.computeUnitLimit;
    } else {
      units = kDefaultUnitsPerInstruction * nonBudgetInstructions;
    }
    if (units > kMaxComputeUnits) units = kMaxComputeUnits;
    // ceil(price * units / 1e6), split so nothing overflows (units <= 1.4M).
    const uint64_t price = *s.computeUnitPriceMicroLamports;
    const uint64_t whole = price / 1000000;
    const uint64_t rest = price % 1000000;
    if (units != 0 && whole > (UINT64_MAX - 2000000) / units) {
      s.maxPriorityFeeLamports = UINT64_MAX;
    } else {
      s.maxPriorityFeeLamports = whole * units + (rest * units + 999999) / 1000000;
    }
  }
  return s;
}

}  // namespace wfw::solana
