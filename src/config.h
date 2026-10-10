#pragma once

#include <cstdint>
#include <string>

namespace wfw {

// Runtime configuration. Defaults match the wiring in build manual Step 1;
// every field can be overridden from /etc/wallet-fw/config.json (keys are the
// snake_case names in config.cpp) and a few from the command line.
struct Config {
  // Serial link (Step 2): USB gadget by default, /dev/serial0 for the Pico fallback.
  std::string serialDevice = "/dev/ttyGS0";
  int serialBaud = 115200;

  std::string dataDir = "/var/lib/wallet-fw";
  std::string deviceName;  // empty: "SolPi-" + first 4 chars of the address

  // Grove LCD RGB backlight on I2C bus 1 (pins 3/5).
  std::string i2cBus = "/dev/i2c-1";
  int lcdAddress = 0x3e;
  int backlightAddress = -1;  // -1 = probe 0x62 (v4) then 0x30 (v5)
  int backlightBrightness = 100;  // percent; lower it if the Pi browns out

  // Buttons and buzzer on gpiochip0 (BCM numbering).
  std::string gpioChip = "gpiochip0";
  int gpioLeft = 17;
  int gpioMiddle = 27;
  int gpioRight = 22;
  int gpioBuzzer = 23;
  bool buttonsActiveHigh = true;
  int debounceMs = 30;
  int holdConfirmMs = 2000;
  bool buzzerEnabled = true;

  // Device risk rule (Step 7) and signing behaviour.
  uint64_t riskLimitLamports = 1000000000ULL;  // 1 SOL
  uint64_t maxPriorityFeeLamports = 10000000ULL;  // 0.01 SOL
  bool allowBlindSigning = true;  // allow approving "UNKNOWN TX" (always red)
  int reviewTimeoutS = 180;
  int setupTimeoutS = 900;

  // Hardening (Step 12), off by default so the app can integrate first.
  bool pinEnabled = false;
  int pinLength = 4;
  int pinMaxAttempts = 5;  // per boot; then the device refuses until restart
  bool requireSecureChannel = false;
};

// Loads a JSON config file over `cfg`. A missing file is not an error when
// `optional` is set. Unknown keys are logged and ignored; out-of-range
// values are errors.
bool loadConfigFile(const std::string& path, Config& cfg, bool optional, std::string& err);

// Same as above but from JSON text (used by tests).
bool applyConfigJson(const std::string& text, Config& cfg, std::string& err);

}  // namespace wfw
