#pragma once

#include <array>
#include <cstdint>
#include <string>

namespace wfw::hal {

// 16x2 character display with an RGB backlight (build manual Step 3:
// clear, print(row, text), setRGB(r, g, b)). Methods return false on a bus
// error; the caller re-initialises and redraws.
class Display {
 public:
  virtual ~Display() = default;
  virtual bool init() = 0;
  virtual bool clear() = 0;
  virtual bool print(int row, const std::string& text) = 0;
  virtual bool setRGB(uint8_t r, uint8_t g, uint8_t b) = 0;
};

// Raw (undebounced) button levels, already converted to "pressed".
// Index 0 = left (back/reject), 1 = middle (next/scroll), 2 = right (confirm).
class ButtonReader {
 public:
  virtual ~ButtonReader() = default;
  virtual bool read(std::array<bool, 3>& pressed) = 0;
};

class Buzzer {
 public:
  virtual ~Buzzer() = default;
  virtual void set(bool on) = 0;
};

}  // namespace wfw::hal
