#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace wfw {

using Bytes = std::vector<uint8_t>;
using PubKey = std::array<uint8_t, 32>;

std::string base58Encode(const uint8_t* data, size_t len);
inline std::string base58Encode(const PubKey& key) { return base58Encode(key.data(), key.size()); }
// Strict Bitcoin-alphabet decode; rejects any character outside the alphabet.
std::optional<Bytes> base58Decode(const std::string& text);
// Decodes and requires exactly 32 bytes (a Solana address).
std::optional<PubKey> decodeAddress(const std::string& text);

// Standard (RFC 4648) base64 with padding, as produced by JS Buffer/btoa.
std::string base64Encode(const uint8_t* data, size_t len);
inline std::string base64Encode(const Bytes& b) { return base64Encode(b.data(), b.size()); }
std::optional<Bytes> base64Decode(const std::string& text);

std::string hexEncode(const uint8_t* data, size_t len);
std::optional<Bytes> hexDecode(const std::string& text);

}  // namespace wfw
