#include "hal/linux/grove_lcd.h"

#include <unistd.h>

#include "util/log.h"

namespace wfw::hal {
namespace {

// HD44780 command bits (rgb_lcd.h).
constexpr uint8_t kClearDisplay = 0x01;
constexpr uint8_t kEntryModeSet = 0x04;
constexpr uint8_t kEntryLeft = 0x02;
constexpr uint8_t kDisplayControl = 0x08;
constexpr uint8_t kDisplayOn = 0x04;
constexpr uint8_t kFunctionSet = 0x20;
constexpr uint8_t kTwoLine = 0x08;
constexpr uint8_t kSetDdramAddr = 0x80;

constexpr uint8_t kBacklightV4 = 0x62;
constexpr uint8_t kBacklightV5 = 0x30;

}  // namespace

bool GroveLcd::command(uint8_t value) {
  const uint8_t buf[2] = {0x80, value};
  return bus_.write(lcdAddress_, buf, sizeof(buf));
}

bool GroveLcd::data(uint8_t value) {
  const uint8_t buf[2] = {0x40, value};
  return bus_.write(lcdAddress_, buf, sizeof(buf));
}

bool GroveLcd::setReg(uint8_t reg, uint8_t value) {
  const uint8_t buf[2] = {reg, value};
  return bus_.write(backlightAddress_, buf, sizeof(buf));
}

bool GroveLcd::init() {
  if (!bus_.open()) return false;
  // Power-on sequence from rgb_lcd::begin(): function set three times.
  const uint8_t function = kFunctionSet | kTwoLine;
  // The 50 ms power-on wait is only needed once; re-inits during error
  // recovery must not stall the 5 ms button sampling loop that long.
  if (!poweredUp_) ::usleep(50000);
  poweredUp_ = true;
  if (!command(function)) {
    LOG_ERROR("lcd: no ACK at 0x%02x on %s (check wiring: SDA pin 3, SCL pin 5)", lcdAddress_,
              bus_.path().c_str());
    return false;
  }
  ::usleep(4500);
  command(function);
  ::usleep(150);
  command(function);
  const bool ok = command(function) && command(kDisplayControl | kDisplayOn) && clear() &&
                  command(kEntryModeSet | kEntryLeft);
  if (!ok) return false;
  if (!initBacklight() && !warnedNoBacklight_) {
    warnedNoBacklight_ = true;
    LOG_WARN("lcd: no RGB backlight found at 0x62 or 0x30; risk colours will not show");
  }
  return true;
}

bool GroveLcd::initBacklight() {
  backlight_ = Backlight::None;
  if (configuredBacklight_ >= 0) {
    backlightAddress_ = static_cast<uint8_t>(configuredBacklight_);
    backlight_ = backlightAddress_ == kBacklightV5 ? Backlight::V5 : Backlight::V4;
  } else if (bus_.probe(kBacklightV4)) {
    backlightAddress_ = kBacklightV4;
    backlight_ = Backlight::V4;
  } else if (bus_.probe(kBacklightV5)) {
    backlightAddress_ = kBacklightV5;
    backlight_ = Backlight::V5;
  } else {
    return false;
  }
  if (backlight_ == Backlight::V5) {
    if (!setReg(0x00, 0x07)) return false;  // reset the chip
    ::usleep(200);
    return setReg(0x04, 0x15);  // all LEDs always on, PWM controlled
  }
  // PCA9633: normal mode, all outputs PWM-controlled, group dimming.
  return setReg(0x00, 0x00) && setReg(0x08, 0xFF) && setReg(0x01, 0x20);
}

bool GroveLcd::clear() {
  if (!command(kClearDisplay)) return false;
  ::usleep(2000);  // clear takes ~1.5 ms
  return true;
}

bool GroveLcd::print(int row, const std::string& text) {
  if (!command(kSetDdramAddr | (row == 0 ? 0x00 : 0x40))) return false;
  for (size_t i = 0; i < text.size() && i < 16; ++i) {
    if (!data(static_cast<uint8_t>(text[i]))) return false;
  }
  return true;
}

bool GroveLcd::setRGB(uint8_t r, uint8_t g, uint8_t b) {
  switch (backlight_) {
    case Backlight::None: return true;  // text still works without colour
    case Backlight::V5: return setReg(0x06, r) && setReg(0x07, g) && setReg(0x08, b);
    case Backlight::V4: return setReg(0x04, r) && setReg(0x03, g) && setReg(0x02, b);
  }
  return false;
}

}  // namespace wfw::hal
