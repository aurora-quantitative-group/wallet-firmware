#include "solana/message.h"

#include <algorithm>
#include <set>

namespace wfw::solana {

bool Message::isWritable(size_t index) const {
  if (index < numRequiredSignatures) {
    return index < static_cast<size_t>(numRequiredSignatures - numReadonlySigned);
  }
  if (index < accountKeys.size()) {
    return index < accountKeys.size() - numReadonlyUnsigned;
  }
  return false;  // lookup-table accounts: writability is not tracked here
}

bool readCompactU16(const uint8_t* data, size_t len, size_t& pos, uint16_t& value) {
  uint32_t result = 0;
  for (int i = 0; i < 3; ++i) {
    if (pos >= len) return false;
    const uint8_t b = data[pos++];
    if (i > 0 && b == 0) return false;  // non-canonical: redundant zero byte
    if (i == 2 && b > 0x03) return false;  // would exceed 16 bits
    result |= static_cast<uint32_t>(b & 0x7f) << (7 * i);
    if ((b & 0x80) == 0) {
      value = static_cast<uint16_t>(result);
      return true;
    }
  }
  return false;
}

namespace {

class Reader {
 public:
  Reader(const uint8_t* data, size_t len) : data_(data), len_(len) {}

  bool u8(uint8_t& v) {
    if (pos_ >= len_) return false;
    v = data_[pos_++];
    return true;
  }
  bool compact(uint16_t& v) { return readCompactU16(data_, len_, pos_, v); }
  bool bytes(uint8_t* out, size_t n) {
    if (len_ - pos_ < n) return false;
    std::copy(data_ + pos_, data_ + pos_ + n, out);
    pos_ += n;
    return true;
  }
  bool vec(Bytes& out, size_t n) {
    if (len_ - pos_ < n) return false;
    out.assign(data_ + pos_, data_ + pos_ + n);
    pos_ += n;
    return true;
  }
  bool atEnd() const { return pos_ == len_; }
  size_t remaining() const { return len_ - pos_; }

 private:
  const uint8_t* data_;
  size_t len_;
  size_t pos_ = 0;
};

}  // namespace

bool parseMessage(const uint8_t* data, size_t len, Message& out, std::string& err) {
  out = Message{};
  if (len == 0 || len > kMaxMessageBytes) {
    err = "message size out of range";
    return false;
  }
  Reader r(data, len);

  uint8_t first = 0;
  r.u8(first);
  if (first & 0x80) {
    out.versioned = true;
    out.version = first & 0x7f;
    if (out.version != 0) {
      err = "unsupported message version";
      return false;
    }
    if (!r.u8(out.numRequiredSignatures)) {
      err = "truncated header";
      return false;
    }
  } else {
    out.numRequiredSignatures = first;
  }
  if (!r.u8(out.numReadonlySigned) || !r.u8(out.numReadonlyUnsigned)) {
    err = "truncated header";
    return false;
  }

  uint16_t numKeys = 0;
  if (!r.compact(numKeys)) {
    err = "bad account key count";
    return false;
  }
  if (numKeys == 0 || numKeys > len / 32) {
    err = "bad account key count";
    return false;
  }
  out.accountKeys.resize(numKeys);
  for (auto& key : out.accountKeys) {
    if (!r.bytes(key.data(), key.size())) {
      err = "truncated account keys";
      return false;
    }
  }
  // Same rules as Solana's message sanitize().
  if (out.numRequiredSignatures == 0 || out.numRequiredSignatures > numKeys ||
      out.numReadonlySigned >= out.numRequiredSignatures ||
      out.numReadonlyUnsigned > numKeys - out.numRequiredSignatures) {
    err = "inconsistent header";
    return false;
  }
  if (std::set<PubKey>(out.accountKeys.begin(), out.accountKeys.end()).size() != numKeys) {
    err = "duplicate account keys";
    return false;
  }

  if (!r.bytes(out.recentBlockhash.data(), out.recentBlockhash.size())) {
    err = "truncated blockhash";
    return false;
  }

  uint16_t numInstructions = 0;
  // Each instruction takes at least 3 bytes; check before allocating.
  if (!r.compact(numInstructions) || numInstructions > r.remaining() / 3) {
    err = "bad instruction count";
    return false;
  }
  out.instructions.resize(numInstructions);
  for (auto& ix : out.instructions) {
    uint16_t n = 0;
    if (!r.u8(ix.programIdIndex) || !r.compact(n) || !r.vec(ix.accounts, n) || !r.compact(n) ||
        !r.vec(ix.data, n)) {
      err = "truncated instruction";
      return false;
    }
  }

  if (out.versioned) {
    uint16_t numLookups = 0;
    if (!r.compact(numLookups)) {
      err = "bad lookup table count";
      return false;
    }
    out.lookupTableCount = numLookups;
    for (uint16_t i = 0; i < numLookups; ++i) {
      PubKey table{};
      Bytes writable;
      Bytes readonly;
      uint16_t n = 0;
      if (!r.bytes(table.data(), table.size()) || !r.compact(n) || !r.vec(writable, n) ||
          !r.compact(n) || !r.vec(readonly, n)) {
        err = "truncated lookup table";
        return false;
      }
      out.lookupAccountCount += writable.size() + readonly.size();
    }
  }

  if (!r.atEnd()) {
    err = "trailing bytes after message";
    return false;
  }
  if (out.totalAccounts() > 256) {
    err = "too many accounts";
    return false;
  }

  for (const auto& ix : out.instructions) {
    // The fee payer (index 0) can never be a program; programs cannot come
    // from lookup tables.
    if (ix.programIdIndex == 0 || ix.programIdIndex >= out.accountKeys.size()) {
      err = "bad program index";
      return false;
    }
    for (const uint8_t a : ix.accounts) {
      if (a >= out.totalAccounts()) {
        err = "bad account index";
        return false;
      }
    }
  }
  return true;
}

}  // namespace wfw::solana
