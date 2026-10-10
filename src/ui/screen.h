#pragma once

#include <cstdint>
#include <string>

#include "hal/interfaces.h"
#include "util/clock.h"

namespace wfw::ui {

struct Rgb {
  uint8_t r = 0, g = 0, b = 0;
  bool operator==(const Rgb& o) const { return r == o.r && g == o.g && b == o.b; }
  bool operator!=(const Rgb& o) const { return !(*this == o); }
};

namespace colors {
constexpr Rgb kOff{0, 0, 0};
constexpr Rgb kIdle{40, 40, 60};
constexpr Rgb kSetup{0, 90, 255};
constexpr Rgb kLow{0, 255, 0};       // green
constexpr Rgb kMedium{255, 180, 0};  // yellow
constexpr Rgb kHigh{255, 0, 0};      // red
}  // namespace colors

struct Screen {
  std::string top;
  std::string bottom;
  Rgb color = colors::kIdle;
  bool operator==(const Screen& o) const {
    return top == o.top && bottom == o.bottom && color == o.color;
  }
};

// Owns the physical display: pads/sanitizes lines, only writes rows that
// changed, scales the backlight, and recovers from I2C errors by
// re-initialising and redrawing (at most once per second).
class ScreenRenderer {
 public:
  ScreenRenderer(hal::Display& display, const Clock& clock, int brightnessPercent);

  bool begin();  // initialise the panel; false if it did not respond
  void show(const Screen& screen);
  void retryIfNeeded();
  bool healthy() const { return healthy_; }
  const Screen& current() const { return wanted_; }

 private:
  bool draw(bool full);

  hal::Display& display_;
  const Clock& clock_;
  int brightness_;
  Screen wanted_;
  Screen drawn_;
  bool healthy_ = false;
  bool everDrawn_ = false;
  TimeMs lastAttempt_ = 0;
};

}  // namespace wfw::ui
