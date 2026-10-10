#pragma once

#include <cstdint>
#include <string>

namespace wfw {

constexpr uint64_t kLamportsPerSol = 1000000000ULL;
constexpr size_t kLcdCols = 16;

// Exact decimal SOL amount with at least `minDecimals` places and trailing
// zeros trimmed beyond that: 500000000 -> "0.50", 5000 -> "0.000005".
std::string formatSol(uint64_t lamports, size_t minDecimals = 2);

// "7xKXtg2CW87d97TXJSDpbD5jBkheTqA83TZRuJosgAsU" -> "7xKX..gAsU"
std::string abbreviateAddress(const std::string& address);

// Replaces anything the HD44780 character ROM cannot show (outside 0x20-0x7D,
// plus '\\' which renders as a yen sign) with '?'.
std::string lcdSanitize(const std::string& text);

// Sanitizes, then truncates or right-pads with spaces to exactly `cols`.
std::string lcdLine(const std::string& text, size_t cols = kLcdCols);

}  // namespace wfw
