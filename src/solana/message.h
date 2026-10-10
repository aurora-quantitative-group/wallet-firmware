#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "crypto/encoding.h"

namespace wfw::solana {

// Largest transaction Solana accepts (PACKET_DATA_SIZE); a message is smaller.
constexpr size_t kMaxMessageBytes = 1232;

struct CompiledInstruction {
  uint8_t programIdIndex = 0;
  std::vector<uint8_t> accounts;  // indices into the full account list
  Bytes data;
};

// A parsed legacy or v0 transaction message (the bytes that get signed).
struct Message {
  bool versioned = false;  // false = legacy
  uint8_t version = 0;
  uint8_t numRequiredSignatures = 0;
  uint8_t numReadonlySigned = 0;
  uint8_t numReadonlyUnsigned = 0;
  std::vector<PubKey> accountKeys;  // static keys only
  PubKey recentBlockhash{};
  std::vector<CompiledInstruction> instructions;
  size_t lookupTableCount = 0;
  size_t lookupAccountCount = 0;  // accounts loaded from lookup tables (unknown to us)

  size_t totalAccounts() const { return accountKeys.size() + lookupAccountCount; }
  bool isSigner(size_t index) const { return index < numRequiredSignatures; }
  bool isWritable(size_t index) const;
  // True when the index refers to a static key we can display.
  bool isStatic(size_t index) const { return index < accountKeys.size(); }
};

// Parses and sanity-checks a message. Rejects truncated or trailing data,
// non-canonical compact-u16 lengths, out-of-range indices, duplicate keys and
// unsupported versions.
bool parseMessage(const uint8_t* data, size_t len, Message& out, std::string& err);

// Exposed for tests: decodes Solana's compact-u16 ("shortvec") encoding.
bool readCompactU16(const uint8_t* data, size_t len, size_t& pos, uint16_t& value);

}  // namespace wfw::solana
