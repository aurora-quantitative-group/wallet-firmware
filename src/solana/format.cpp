#include "solana/format.h"

namespace wfw {

std::string formatSol(uint64_t lamports, size_t minDecimals) {
  const uint64_t whole = lamports / kLamportsPerSol;
  const uint64_t frac = lamports % kLamportsPerSol;
  std::string fracDigits = std::to_string(frac);
  fracDigits.insert(0, 9 - fracDigits.size(), '0');
  if (minDecimals > 9) minDecimals = 9;
  size_t keep = 9;
  while (keep > minDecimals && fracDigits[keep - 1] == '0') --keep;
  fracDigits.resize(keep);
  std::string out = std::to_string(whole);
  if (!fracDigits.empty()) out += "." + fracDigits;
  return out;
}

std::string abbreviateAddress(const std::string& address) {
  if (address.size() <= 10) return address;
  return address.substr(0, 4) + ".." + address.substr(address.size() - 4);
}

std::string lcdSanitize(const std::string& text) {
  std::string out;
  out.reserve(text.size());
  for (const char ch : text) {
    const auto c = static_cast<unsigned char>(ch);
    out.push_back((c >= 0x20 && c <= 0x7d && c != '\\') ? ch : '?');
  }
  return out;
}

std::string lcdLine(const std::string& text, size_t cols) {
  std::string out = lcdSanitize(text);
  out.resize(cols, ' ');
  return out;
}

}  // namespace wfw
