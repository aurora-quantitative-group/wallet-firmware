#pragma once

#include <array>
#include <memory>
#include <string>

#include "hal/interfaces.h"

namespace wfw::hal {

// Three Grove buttons (inputs) and the Grove buzzer (output) through
// libgpiod. Builds against libgpiod 1.6 (Pi OS Bookworm) or 2.x (Trixie).
class GpiodIo final : public ButtonReader, public Buzzer {
 public:
  struct Pins {
    std::string chip;  // "gpiochip0" or "/dev/gpiochip0"
    std::array<unsigned, 3> buttons;  // left, middle, right
    unsigned buzzer;
    bool activeHigh;
  };

  explicit GpiodIo(Pins pins);
  ~GpiodIo() override;

  bool open(std::string& err);
  bool read(std::array<bool, 3>& pressed) override;
  void set(bool on) override;

 private:
  struct Impl;
  Pins pins_;
  std::unique_ptr<Impl> impl_;
};

}  // namespace wfw::hal
