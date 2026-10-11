#include "hw_test.h"

#include <unistd.h>

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

void showPage(hal::GroveLcd& lcd, int page) {
  lcd.clear();
  lcd.print(0, "Page " + std::to_string(page));
  lcd.print(1, "M: next page");
  std::printf("Page %d\n", page);
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

  hal::GpiodIo gpio({cfg.gpioChip,
                     {static_cast<unsigned>(cfg.gpioLeft), static_cast<unsigned>(cfg.gpioMiddle),
                      static_cast<unsigned>(cfg.gpioRight)},
                     static_cast<unsigned>(cfg.gpioBuzzer),
                     cfg.buttonsActiveHigh});
  std::string err;
  const bool haveGpio = gpio.open(err);
  if (!haveGpio) {
    std::printf("GPIO: %s (is the wallet-fw service still running?)\n", err.c_str());
  } else {
    std::printf("GPIO: buttons on %d/%d/%d, buzzer on %d. Press buttons; Ctrl+C to quit.\n",
                cfg.gpioLeft, cfg.gpioMiddle, cfg.gpioRight, cfg.gpioBuzzer);
  }

  int page = 1;
  showPage(lcd, page);
  SteadyClock clock;
  ui::ButtonInput input(cfg.debounceMs, cfg.holdConfirmMs);
  std::vector<ui::ButtonEvent> events;
  static const char* const kNames[] = {"left", "middle", "right"};
  TimeMs buzzerOffAt = 0;

  while (!stop) {
    std::array<bool, 3> raw{};
    if (haveGpio && gpio.read(raw)) {
      events.clear();
      input.update(raw, clock.nowMs(), events);
      for (const auto& ev : events) {
        if (ev.kind != ui::ButtonEvent::Kind::Press) continue;
        std::printf("button: %s\n", kNames[static_cast<int>(ev.button)]);
        if (ev.button == ui::Button::Middle) {
          page = page % 3 + 1;  // Page 1 -> 2 -> 3 -> 1, like the Arduino example
          showPage(lcd, page);
        } else if (ev.button == ui::Button::Right) {
          gpio.set(true);
          buzzerOffAt = clock.nowMs() + 150;
        }
      }
    }
    if (buzzerOffAt != 0 && clock.nowMs() >= buzzerOffAt) {
      gpio.set(false);
      buzzerOffAt = 0;
    }
    std::fflush(stdout);
    ::usleep(5000);
  }
  lcd.clear();
  lcd.setRGB(0, 0, 0);
  return 0;
}

}  // namespace wfw
