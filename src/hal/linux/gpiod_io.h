#pragma once

#include <array>
#include <memory>
#include <string>
#include <vector>

#include "hal/interfaces.h"

namespace wfw::hal {

// Grove buttons (inputs) and the Grove buzzer (output) through libgpiod.
// Builds against libgpiod 1.6 (Pi OS Bookworm) or 2.x (Trixie).
class GpiodIo final : public ButtonReader, public Buzzer {
 public:
  struct Pins {
    std::string chip;  // "gpiochip0" or "/dev/gpiochip0"
    std::vector<int> buttons;  // left, middle, right; -1 = not fitted
    int buzzer = -1;           // -1 = none
    bool activeHigh = true;
  };

  explicit GpiodIo(Pins pins);
  ~GpiodIo() override;

  bool open(std::string& err);
  bool read(std::array<bool, 3>& pressed) override;  // the first three buttons
  bool readAll(std::vector<bool>& pressed);          // every entry of Pins::buttons
  void set(bool on) override;

 private:
  struct Impl;
  Pins pins_;
  std::vector<unsigned> inputs_;  // the fitted button lines, in order
  std::unique_ptr<Impl> impl_;
};

}  // namespace wfw::hal
