#include "hw_test.h"

#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include "hal/linux/gpiod_io.h"
#include "hal/linux/grove_lcd.h"
#include "hal/linux/i2c_bus.h"
#include "ui/button_input.h"
#include "util/clock.h"

namespace wfw {

namespace {

// The GPIOs on the Grove Base HAT's digital ports (D5, D16, D18, D22, D24,
// D26) and PWM port (12/13); every port carries two. I2C and UART pins are
// left alone so the LCD keeps working.
const std::vector<int> kCandidates = {5, 6, 12, 13, 16, 17, 18, 19, 22, 23, 24, 25, 26, 27};

void show(hal::GroveLcd& lcd, const std::string& top, const std::string& bottom) {
  lcd.clear();
  lcd.print(0, top);
  lcd.print(1, bottom);
  std::printf("LCD: %s | %s\n", top.c_str(), bottom.c_str());
  std::fflush(stdout);
}

// Waits until one line that is not in `skip` stays pressed for 50 ms and is
// released again; returns its GPIO number (-1 when stopped).
int waitForButton(hal::GpiodIo& gpio, const std::vector<int>& pins, const std::vector<int>& skip,
                  volatile std::sig_atomic_t& stop) {
  SteadyClock clock;
  std::vector<bool> raw;
  int candidate = -1;
  TimeMs since = 0;
  while (!stop) {
    ::usleep(5000);
    if (!gpio.readAll(raw)) continue;
    int down = -1;
    for (size_t i = 0; i < pins.size() && down < 0; ++i) {
      if (raw[i] && std::find(skip.begin(), skip.end(), pins[i]) == skip.end()) down = static_cast<int>(i);
    }
    if (down != candidate) {
      candidate = down;
      since = clock.nowMs();
    } else if (down >= 0 && clock.nowMs() - since >= 50) {
      while (!stop && gpio.readAll(raw) && raw[static_cast<size_t>(down)]) ::usleep(5000);
      return pins[static_cast<size_t>(down)];
    }
  }
  return -1;
}

}  // namespace

int runHardwareTest(const Config& cfg, volatile std::sig_atomic_t& stop) {
  hal::I2cBus bus(cfg.i2cBus);
  hal::GroveLcd lcd(bus, cfg.lcdAddress, cfg.backlightAddress);
  if (!lcd.init()) {
    std::printf("LCD: no answer on %s at 0x%02x. Check the cable is in an I2C socket.\n",
                cfg.i2cBus.c_str(), cfg.lcdAddress);
    return 1;
  }
  lcd.setRGB(255, 255, 255);
  std::printf("LCD: OK (if the backlight is on but no text shows, the LCD needs 5 V)\n");

  std::vector<int> pins;
  for (int pin : kCandidates) {
    if (pin != cfg.gpioBuzzer) pins.push_back(pin);
  }
  hal::GpiodIo gpio({cfg.gpioChip, pins, cfg.gpioBuzzer, cfg.buttonsActiveHigh});
  std::string err;
  if (!gpio.open(err)) {
    std::printf("GPIO: %s (stop the service first: sudo systemctl stop wallet-fw)\n", err.c_str());
    return 1;
  }
  // A line that already reads "pressed" with nothing touched is not a button.
  std::vector<int> skip;
  std::vector<bool> raw;
  ::usleep(20000);
  if (gpio.readAll(raw)) {
    for (size_t i = 0; i < pins.size(); ++i) {
      if (raw[i]) skip.push_back(pins[i]);
    }
  }
  for (int pin : skip) std::printf("GPIO %d reads pressed at rest; ignoring it\n", pin);

  show(lcd, "BUTTON FINDER", "PRESS LEFT BTN");
  const int left = waitForButton(gpio, pins, skip, stop);
  if (left < 0) return 0;
  skip.push_back(left);
  show(lcd, "LEFT: GPIO " + std::to_string(left), "PRESS RIGHT BTN");
  const int right = waitForButton(gpio, pins, skip, stop);
  if (right < 0) return 0;
  std::printf("\nYour buttons: left GPIO %d, right GPIO %d.\n", left, right);
  if (left == cfg.gpioLeft && right == cfg.gpioRight) {
    std::printf("They match the config. Nothing to change.\n");
  } else {
    std::printf("Put these lines in /etc/wallet-fw/config.json, then restart wallet-fw:\n"
                "  \"gpio_left\": %d,\n  \"gpio_right\": %d,\n  \"gpio_middle\": -1,\n",
                left, right);
  }

  // Practice with the two-button layout: tap right for next, hold to confirm.
  const size_t li = static_cast<size_t>(std::find(pins.begin(), pins.end(), left) - pins.begin());
  const size_t ri = static_cast<size_t>(std::find(pins.begin(), pins.end(), right) - pins.begin());
  SteadyClock clock;
  ui::ButtonInput input(cfg.debounceMs, cfg.holdConfirmMs);
  std::vector<ui::ButtonEvent> events;
  int page = 1;
  TimeMs buzzerOffAt = 0;
  show(lcd, "TAP R: NEXT", "HOLD R: BEEP");
  while (!stop) {
    ::usleep(5000);
    if (buzzerOffAt != 0 && clock.nowMs() >= buzzerOffAt) {
      gpio.set(false);
      buzzerOffAt = 0;
    }
    if (!gpio.readAll(raw)) continue;
    events.clear();
    input.update({raw[li], false, raw[ri]}, clock.nowMs(), events);
    for (const auto& ev : events) {
      using Kind = ui::ButtonEvent::Kind;
      if (ev.button == ui::Button::Left && ev.kind == Kind::Press) {
        show(lcd, "LEFT: BACK", "TAP R: NEXT");
      } else if (ev.button == ui::Button::Right && ev.kind == Kind::Hold) {
        show(lcd, "HOLD: CONFIRMED", "BEEP");
        gpio.set(true);
        buzzerOffAt = clock.nowMs() + 300;
      } else if (ev.button == ui::Button::Right && ev.kind == Kind::Release &&
                 ev.at - ev.pressedAt < input.holdMs()) {
        page = page % 3 + 1;
        show(lcd, "PAGE " + std::to_string(page), "HOLD R: BEEP");
      }
    }
  }
  lcd.clear();
  lcd.setRGB(0, 0, 0);
  return 0;
}

}  // namespace wfw
