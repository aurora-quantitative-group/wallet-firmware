#pragma once

#include "hal/interfaces.h"
#include "hal/linux/i2c_bus.h"

namespace wfw::hal {

// Seeed Grove LCD RGB Backlight (16x2). Command set ported from Seeed's
// Grove_LCD_RGB_Backlight Arduino library (rgb_lcd.cpp):
//  - text controller (AiP31068 / HD44780-compatible) at 0x3E: control byte
//    0x80 precedes a command, 0x40 precedes a data (character) byte;
//  - backlight at 0x62 (PCA9633, v4.0) or 0x30 (v5.0), probed at init.
class GroveLcd final : public Display {
 public:
  GroveLcd(I2cBus& bus, int lcdAddress, int backlightAddress)
      : bus_(bus), lcdAddress_(static_cast<uint8_t>(lcdAddress)), configuredBacklight_(backlightAddress) {}

  bool init() override;
  bool clear() override;
  bool print(int row, const std::string& text) override;
  bool setRGB(uint8_t r, uint8_t g, uint8_t b) override;

 private:
  enum class Backlight { None, V4, V5 };

  bool command(uint8_t value);
  bool data(uint8_t value);
  bool setReg(uint8_t reg, uint8_t value);
  bool initBacklight();

  I2cBus& bus_;
  uint8_t lcdAddress_;
  int configuredBacklight_;
  Backlight backlight_ = Backlight::None;
  uint8_t backlightAddress_ = 0;
  bool warnedNoBacklight_ = false;
};

}  // namespace wfw::hal
