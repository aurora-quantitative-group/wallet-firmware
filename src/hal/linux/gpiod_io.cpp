#include "hal/linux/gpiod_io.h"

#include <gpiod.h>

#include <cerrno>
#include <cstring>

#include "util/log.h"

namespace wfw::hal {

namespace {
constexpr const char* kConsumer = "wallet-fw";
}

#if defined(WFW_GPIOD_V2)
// ------------------------------------------------------------ libgpiod 2.x --

struct GpiodIo::Impl {
  gpiod_chip* chip = nullptr;
  gpiod_line_request* request = nullptr;
  ~Impl() {
    if (request) gpiod_line_request_release(request);
    if (chip) gpiod_chip_close(chip);
  }
};

bool GpiodIo::open(std::string& err) {
  impl_ = std::make_unique<Impl>();
  const std::string path = pins_.chip.rfind('/', 0) == 0 ? pins_.chip : "/dev/" + pins_.chip;
  impl_->chip = gpiod_chip_open(path.c_str());
  if (!impl_->chip) {
    err = "gpiod: open " + path + ": " + std::strerror(errno);
    return false;
  }
  gpiod_line_settings* in = gpiod_line_settings_new();
  gpiod_line_settings* out = gpiod_line_settings_new();
  gpiod_line_config* lines = gpiod_line_config_new();
  gpiod_request_config* req = gpiod_request_config_new();
  bool ok = in && out && lines && req;
  if (ok) {
    gpiod_line_settings_set_direction(in, GPIOD_LINE_DIRECTION_INPUT);
    gpiod_line_settings_set_bias(in, pins_.activeHigh ? GPIOD_LINE_BIAS_PULL_DOWN
                                                      : GPIOD_LINE_BIAS_PULL_UP);
    gpiod_line_settings_set_direction(out, GPIOD_LINE_DIRECTION_OUTPUT);
    gpiod_line_settings_set_output_value(out, GPIOD_LINE_VALUE_INACTIVE);
    const unsigned buzzer = static_cast<unsigned>(pins_.buzzer);
    ok = (inputs_.empty() ||
          gpiod_line_config_add_line_settings(lines, inputs_.data(), inputs_.size(), in) == 0) &&
         (pins_.buzzer < 0 || gpiod_line_config_add_line_settings(lines, &buzzer, 1, out) == 0);
    gpiod_request_config_set_consumer(req, kConsumer);
  }
  if (ok) impl_->request = gpiod_chip_request_lines(impl_->chip, req, lines);
  ok = ok && impl_->request != nullptr;
  if (!ok) err = std::string("gpiod: request lines: ") + std::strerror(errno);
  if (req) gpiod_request_config_free(req);
  if (lines) gpiod_line_config_free(lines);
  if (out) gpiod_line_settings_free(out);
  if (in) gpiod_line_settings_free(in);
  return ok;
}

bool GpiodIo::readAll(std::vector<bool>& pressed) {
  if (!impl_ || !impl_->request) return false;
  std::vector<gpiod_line_value> values(inputs_.size());
  if (!inputs_.empty() && gpiod_line_request_get_values_subset(impl_->request, inputs_.size(),
                                                               inputs_.data(), values.data()) != 0) {
    return false;
  }
  pressed.assign(pins_.buttons.size(), false);
  for (size_t i = 0, line = 0; i < pins_.buttons.size(); ++i) {
    if (pins_.buttons[i] < 0) continue;
    const bool high = values[line++] == GPIOD_LINE_VALUE_ACTIVE;
    pressed[i] = pins_.activeHigh ? high : !high;
  }
  return true;
}

void GpiodIo::set(bool on) {
  if (!impl_ || !impl_->request || pins_.buzzer < 0) return;
  gpiod_line_request_set_value(impl_->request, static_cast<unsigned>(pins_.buzzer),
                               on ? GPIOD_LINE_VALUE_ACTIVE : GPIOD_LINE_VALUE_INACTIVE);
}

#else
// ------------------------------------------------------------ libgpiod 1.x --

struct GpiodIo::Impl {
  gpiod_chip* chip = nullptr;
  gpiod_line_bulk buttons;
  gpiod_line* buzzer = nullptr;
  bool buttonsRequested = false;
  ~Impl() {
    if (buttonsRequested) gpiod_line_release_bulk(&buttons);
    if (buzzer) gpiod_line_release(buzzer);
    if (chip) gpiod_chip_close(chip);
  }
};

bool GpiodIo::open(std::string& err) {
  impl_ = std::make_unique<Impl>();
  impl_->chip = gpiod_chip_open_lookup(pins_.chip.c_str());
  if (!impl_->chip) {
    err = "gpiod: open " + pins_.chip + ": " + std::strerror(errno);
    return false;
  }
  if (gpiod_chip_get_lines(impl_->chip, inputs_.data(), static_cast<unsigned>(inputs_.size()), &impl_->buttons) != 0) {
    err = std::string("gpiod: get button lines: ") + std::strerror(errno);
    return false;
  }
  const int bias = pins_.activeHigh ? GPIOD_LINE_REQUEST_FLAG_BIAS_PULL_DOWN
                                    : GPIOD_LINE_REQUEST_FLAG_BIAS_PULL_UP;
  if (gpiod_line_request_bulk_input_flags(&impl_->buttons, kConsumer, bias) != 0) {
    err = std::string("gpiod: request buttons: ") + std::strerror(errno);
    return false;
  }
  impl_->buttonsRequested = true;
  if (pins_.buzzer < 0) return true;
  gpiod_line* buzzer = gpiod_chip_get_line(impl_->chip, static_cast<unsigned>(pins_.buzzer));
  if (!buzzer || gpiod_line_request_output(buzzer, kConsumer, 0) != 0) {
    err = std::string("gpiod: request buzzer: ") + std::strerror(errno);
    return false;
  }
  impl_->buzzer = buzzer;
  return true;
}

bool GpiodIo::readAll(std::vector<bool>& pressed) {
  if (!impl_ || !impl_->buttonsRequested) return false;
  std::vector<int> values(inputs_.size(), 0);
  if (gpiod_line_get_value_bulk(&impl_->buttons, values.data()) != 0) return false;
  pressed.assign(pins_.buttons.size(), false);
  for (size_t i = 0, line = 0; i < pins_.buttons.size(); ++i) {
    if (pins_.buttons[i] < 0) continue;
    const int v = values[line++];
    pressed[i] = pins_.activeHigh ? v == 1 : v == 0;
  }
  return true;
}

void GpiodIo::set(bool on) {
  if (impl_ && impl_->buzzer) gpiod_line_set_value(impl_->buzzer, on ? 1 : 0);
}

#endif

GpiodIo::GpiodIo(Pins pins) : pins_(std::move(pins)) {
  for (int pin : pins_.buttons) {
    if (pin >= 0) inputs_.push_back(static_cast<unsigned>(pin));
  }
}

GpiodIo::~GpiodIo() {
  if (impl_) set(false);
}

bool GpiodIo::read(std::array<bool, 3>& pressed) {
  std::vector<bool> all;
  if (!readAll(all)) return false;
  for (size_t i = 0; i < pressed.size(); ++i) pressed[i] = i < all.size() && all[i];
  return true;
}

}  // namespace wfw::hal
