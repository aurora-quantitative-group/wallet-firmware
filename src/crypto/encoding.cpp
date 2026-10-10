#include "crypto/encoding.h"

#include <sodium.h>

namespace wfw {
namespace {

constexpr char kB58Alphabet[] = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";

int b58Value(char c) {
  for (int i = 0; i < 58; ++i) {
    if (kB58Alphabet[i] == c) return i;
  }
  return -1;
}

}  // namespace

std::string base58Encode(const uint8_t* data, size_t len) {
  size_t zeros = 0;
  while (zeros < len && data[zeros] == 0) ++zeros;

  // log(256)/log(58) ~= 1.37; size the big-endian base58 digit buffer.
  std::vector<uint8_t> digits((len - zeros) * 138 / 100 + 1, 0);
  size_t used = 0;
  for (size_t i = zeros; i < len; ++i) {
    unsigned carry = data[i];
    size_t j = 0;
    for (auto it = digits.rbegin(); (carry != 0 || j < used) && it != digits.rend(); ++it, ++j) {
      carry += 256u * *it;
      *it = static_cast<uint8_t>(carry % 58);
      carry /= 58;
    }
    used = j;
  }
  auto it = digits.begin();
  while (it != digits.end() && *it == 0) ++it;

  std::string out(zeros, '1');
  for (; it != digits.end(); ++it) out.push_back(kB58Alphabet[*it]);
  return out;
}

std::optional<Bytes> base58Decode(const std::string& text) {
  size_t ones = 0;
  while (ones < text.size() && text[ones] == '1') ++ones;

  // log(58)/log(256) ~= 0.733
  std::vector<uint8_t> bytes((text.size() - ones) * 733 / 1000 + 1, 0);
  size_t used = 0;
  for (size_t i = ones; i < text.size(); ++i) {
    const int v = b58Value(text[i]);
    if (v < 0) return std::nullopt;
    unsigned carry = static_cast<unsigned>(v);
    size_t j = 0;
    for (auto it = bytes.rbegin(); (carry != 0 || j < used) && it != bytes.rend(); ++it, ++j) {
      carry += 58u * *it;
      *it = static_cast<uint8_t>(carry & 0xff);
      carry >>= 8;
    }
    if (carry != 0) return std::nullopt;
    used = j;
  }
  auto it = bytes.begin();
  while (it != bytes.end() && *it == 0) ++it;

  Bytes out(ones, 0);
  out.insert(out.end(), it, bytes.end());
  return out;
}

std::optional<PubKey> decodeAddress(const std::string& text) {
  if (text.empty() || text.size() > 44) return std::nullopt;
  auto bytes = base58Decode(text);
  if (!bytes || bytes->size() != 32) return std::nullopt;
  PubKey key{};
  std::copy(bytes->begin(), bytes->end(), key.begin());
  return key;
}

std::string base64Encode(const uint8_t* data, size_t len) {
  const int variant = sodium_base64_VARIANT_ORIGINAL;
  std::string out(sodium_base64_ENCODED_LEN(len, variant), '\0');
  sodium_bin2base64(out.data(), out.size(), data, len, variant);
  out.resize(out.size() - 1);  // drop the terminating NUL
  return out;
}

std::optional<Bytes> base64Decode(const std::string& text) {
  Bytes out(text.size() / 4 * 3 + 3);
  size_t outLen = 0;
  const char* end = nullptr;
  if (sodium_base642bin(out.data(), out.size(), text.data(), text.size(), nullptr, &outLen, &end,
                        sodium_base64_VARIANT_ORIGINAL) != 0) {
    return std::nullopt;
  }
  if (end != text.data() + text.size()) return std::nullopt;  // trailing garbage
  out.resize(outLen);
  return out;
}

std::string hexEncode(const uint8_t* data, size_t len) {
  std::string out(len * 2 + 1, '\0');
  sodium_bin2hex(out.data(), out.size(), data, len);
  out.resize(len * 2);
  return out;
}

std::optional<Bytes> hexDecode(const std::string& text) {
  if (text.size() % 2 != 0) return std::nullopt;
  Bytes out(text.size() / 2);
  size_t outLen = 0;
  const char* end = nullptr;
  if (sodium_hex2bin(out.data(), out.size(), text.data(), text.size(), nullptr, &outLen, &end) != 0 ||
      end != text.data() + text.size()) {
    return std::nullopt;
  }
  out.resize(outLen);
  return out;
}

}  // namespace wfw
