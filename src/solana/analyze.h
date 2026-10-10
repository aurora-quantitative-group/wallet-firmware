#pragma once

#include <optional>
#include <string>
#include <vector>

#include "crypto/encoding.h"
#include "solana/message.h"

namespace wfw::solana {

const PubKey& systemProgramId();         // 11111111111111111111111111111111
const PubKey& computeBudgetProgramId();  // ComputeBudget111111111111111111111111111111
const PubKey& memoProgramId();           // MemoSq4gqABAXKb96qnH8TysNcWxMyWCqXgDLGmfcHr
const PubKey& memoV1ProgramId();         // Memo1UhkJRfHyvLMcVucJwxXeuD728EqVDDwQDxFMNo

struct Transfer {
  PubKey from{};
  PubKey to{};
  uint64_t lamports = 0;
};

struct UnknownInstruction {
  std::optional<PubKey> program;
  std::string reason;  // short, LCD-sized
};

// What the device understood of a message. Anything it could not classify
// lands in `unknown`, which forces the "UNKNOWN TX" red screen.
struct TxSummary {
  PubKey feePayer{};
  std::vector<Transfer> transfers;
  std::vector<std::string> memos;
  std::optional<uint32_t> computeUnitLimit;
  std::optional<uint64_t> computeUnitPriceMicroLamports;
  uint64_t maxPriorityFeeLamports = 0;  // upper bound of the priority fee
  std::vector<UnknownInstruction> unknown;

  bool fullyRecognised() const { return unknown.empty(); }
};

// Recognises System transfers (incl. Solana Pay reference accounts), Compute
// Budget limit/price/heap/data-size instructions and Memo instructions. A
// transfer whose source is not `self` (or not a signer) is treated as unknown.
TxSummary analyze(const Message& msg, const PubKey& self);

}  // namespace wfw::solana
