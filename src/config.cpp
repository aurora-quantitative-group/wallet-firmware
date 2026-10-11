#include "config.h"

#include <nlohmann/json.hpp>

#include <functional>
#include <map>

#include "util/fs.h"
#include "util/log.h"

namespace wfw {

using nlohmann::json;

namespace {

using Setter = std::function<bool(const json&, Config&, std::string&)>;

Setter str(std::string Config::*field) {
  return [field](const json& v, Config& c, std::string& err) {
    if (!v.is_string()) {
      err = "expected a string";
      return false;
    }
    c.*field = v.get<std::string>();
    return true;
  };
}

Setter integer(int Config::*field, int lo, int hi) {
  return [=](const json& v, Config& c, std::string& err) {
    if (!v.is_number_integer() || v.get<int64_t>() < lo || v.get<int64_t>() > hi) {
      err = "expected an integer in [" + std::to_string(lo) + ", " + std::to_string(hi) + "]";
      return false;
    }
    c.*field = v.get<int>();
    return true;
  };
}

Setter u64(uint64_t Config::*field) {
  return [field](const json& v, Config& c, std::string& err) {
    if (!v.is_number_unsigned()) {
      err = "expected a non-negative integer";
      return false;
    }
    c.*field = v.get<uint64_t>();
    return true;
  };
}

Setter boolean(bool Config::*field) {
  return [field](const json& v, Config& c, std::string& err) {
    if (!v.is_boolean()) {
      err = "expected true or false";
      return false;
    }
    c.*field = v.get<bool>();
    return true;
  };
}

const std::map<std::string, Setter>& setters() {
  static const std::map<std::string, Setter> table = {
      {"serial_device", str(&Config::serialDevice)},
      {"serial_baud", integer(&Config::serialBaud, 1200, 4000000)},
      {"tcp_listen", str(&Config::tcpListen)},
      {"data_dir", str(&Config::dataDir)},
      {"device_name", str(&Config::deviceName)},
      {"i2c_bus", str(&Config::i2cBus)},
      {"lcd_address", integer(&Config::lcdAddress, 0x03, 0x77)},
      {"backlight_address", integer(&Config::backlightAddress, -1, 0x77)},
      {"backlight_brightness", integer(&Config::backlightBrightness, 0, 100)},
      {"gpio_chip", str(&Config::gpioChip)},
      {"gpio_left", integer(&Config::gpioLeft, 0, 53)},
      {"gpio_middle", integer(&Config::gpioMiddle, 0, 53)},
      {"gpio_right", integer(&Config::gpioRight, 0, 53)},
      {"gpio_buzzer", integer(&Config::gpioBuzzer, 0, 53)},
      {"buttons_active_high", boolean(&Config::buttonsActiveHigh)},
      {"debounce_ms", integer(&Config::debounceMs, 5, 500)},
      {"hold_confirm_ms", integer(&Config::holdConfirmMs, 500, 10000)},
      {"buzzer_enabled", boolean(&Config::buzzerEnabled)},
      {"risk_limit_lamports", u64(&Config::riskLimitLamports)},
      {"max_priority_fee_lamports", u64(&Config::maxPriorityFeeLamports)},
      {"allow_blind_signing", boolean(&Config::allowBlindSigning)},
      {"review_timeout_s", integer(&Config::reviewTimeoutS, 10, 3600)},
      {"setup_timeout_s", integer(&Config::setupTimeoutS, 30, 7200)},
      {"pin_enabled", boolean(&Config::pinEnabled)},
      {"pin_length", integer(&Config::pinLength, 4, 8)},
      {"pin_max_attempts", integer(&Config::pinMaxAttempts, 1, 20)},
      {"auto_lock_s", integer(&Config::autoLockS, 0, 86400)},
      {"require_secure_channel", boolean(&Config::requireSecureChannel)},
  };
  return table;
}

}  // namespace

bool applyConfigJson(const std::string& text, Config& cfg, std::string& err) {
  const json doc = json::parse(text, nullptr, false, /*ignore_comments=*/true);
  if (doc.is_discarded() || !doc.is_object()) {
    err = "config is not a JSON object";
    return false;
  }
  Config next = cfg;
  for (const auto& [key, value] : doc.items()) {
    const auto it = setters().find(key);
    if (it == setters().end()) {
      LOG_WARN("config: ignoring unknown key '%s'", key.c_str());
      continue;
    }
    std::string why;
    if (!it->second(value, next, why)) {
      err = "config key '" + key + "': " + why;
      return false;
    }
  }
  const int pins[] = {next.gpioLeft, next.gpioMiddle, next.gpioRight, next.gpioBuzzer};
  for (int i = 0; i < 4; ++i) {
    for (int j = i + 1; j < 4; ++j) {
      if (pins[i] == pins[j]) {
        err = "config: two functions share GPIO " + std::to_string(pins[i]);
        return false;
      }
    }
  }
  cfg = next;
  return true;
}

bool loadConfigFile(const std::string& path, Config& cfg, bool optional, std::string& err) {
  std::string text;
  bool missing = false;
  if (!fs::readFile(path, text, err, &missing)) return optional && missing;
  return applyConfigJson(text, cfg, err);
}

}  // namespace wfw
