#include "hal/sim/sim_hal.h"

#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <sstream>

#include <nlohmann/json.hpp>

#include "util/log.h"

namespace wfw::hal {

namespace {

FILE* openLog(const std::string& path) {
  if (path.empty()) return nullptr;
  FILE* f = std::fopen(path.c_str(), "a");
  if (!f) LOG_ERROR("sim: cannot open %s", path.c_str());
  return f;
}

}  // namespace

// ----------------------------------------------------------------- display --

SimDisplay::SimDisplay(const Clock& clock, std::string logPath, bool drawConsole)
    : clock_(clock), log_(openLog(logPath)), drawConsole_(drawConsole) {
  rows_[0] = rows_[1] = std::string(16, ' ');
}

SimDisplay::~SimDisplay() {
  if (log_) std::fclose(log_);
}

bool SimDisplay::clear() {
  rows_[0] = rows_[1] = std::string(16, ' ');
  emit();
  return true;
}

bool SimDisplay::print(int row, const std::string& text) {
  rows_[row == 0 ? 0 : 1] = text;
  emit();
  return true;
}

bool SimDisplay::setRGB(uint8_t r, uint8_t g, uint8_t b) {
  rgb_[0] = r;
  rgb_[1] = g;
  rgb_[2] = b;
  emit();
  return true;
}

void SimDisplay::emit() {
  if (drawConsole_) {
    std::fprintf(stderr, "+----------------+\n|%-16s|  rgb(%u,%u,%u)\n|%-16s|\n+----------------+\n",
                 rows_[0].c_str(), rgb_[0], rgb_[1], rgb_[2], rows_[1].c_str());
  }
  if (log_) {
    const nlohmann::json frame = {{"t", clock_.nowMs()},
                                  {"top", rows_[0]},
                                  {"bottom", rows_[1]},
                                  {"rgb", {rgb_[0], rgb_[1], rgb_[2]}}};
    std::fprintf(log_, "%s\n", frame.dump().c_str());
    std::fflush(log_);
  }
}

// ------------------------------------------------------------------ buzzer --

SimBuzzer::SimBuzzer(const Clock& clock, std::string logPath)
    : clock_(clock), log_(openLog(logPath)) {}

SimBuzzer::~SimBuzzer() {
  if (log_) std::fclose(log_);
}

void SimBuzzer::set(bool on) {
  LOG_DEBUG("sim buzzer %s", on ? "ON" : "off");
  if (log_) {
    std::fprintf(log_, "{\"t\":%lld,\"buzzer\":%s}\n", static_cast<long long>(clock_.nowMs()),
                 on ? "true" : "false");
    std::fflush(log_);
  }
}

// ----------------------------------------------------------------- buttons --

void SimButtons::onReadable() {
  if (fd_ < 0) return;
  char buf[256];
  const ssize_t n = ::read(fd_, buf, sizeof(buf));
  if (n == 0 || (n < 0 && errno != EAGAIN && errno != EINTR)) {
    fd_ = -1;  // EOF: stop polling this input
    return;
  }
  if (n < 0) return;
  partial_.append(buf, static_cast<size_t>(n));
  size_t nl;
  while ((nl = partial_.find('\n')) != std::string::npos) {
    command(partial_.substr(0, nl));
    partial_.erase(0, nl + 1);
  }
}

void SimButtons::command(const std::string& line) {
  std::istringstream in(line);
  std::string verb;
  std::string which;
  long ms = -1;
  in >> verb >> which >> ms;
  auto buttonIndex = [](const std::string& s) {
    if (s == "l" || s == "left") return 0;
    if (s == "m" || s == "middle") return 1;
    if (s == "r" || s == "right") return 2;
    return -1;
  };
  int button = -1;
  TimeMs duration = 100;
  if (verb == "l" || verb == "m" || verb == "r") {
    button = buttonIndex(verb);
    verb = "click";
  } else if (verb == "h") {
    button = 2;
    verb = "hold";
    duration = 2500;
  } else {
    button = buttonIndex(which);
    if (verb == "hold") duration = 2500;
    if (ms > 0) duration = ms;
  }
  if (button < 0 || verb.empty()) {
    if (!verb.empty()) LOG_WARN("sim: unknown button command '%s'", line.c_str());
    return;
  }
  if (verb == "click" || verb == "hold") {
    steps_.push_back({button, true, 60});  // gap so back-to-back clicks debounce
    steps_.push_back({button, false, duration});
  } else if (verb == "press") {
    steps_.push_back({button, true, 0});
  } else if (verb == "release") {
    steps_.push_back({button, false, 0});
  } else {
    LOG_WARN("sim: unknown button command '%s'", line.c_str());
  }
}

bool SimButtons::read(std::array<bool, 3>& pressed) {
  const TimeMs now = clock_.nowMs();
  if (steps_.empty()) lastStepAt_ = now;
  while (!steps_.empty() && now - lastStepAt_ >= steps_.front().afterMs) {
    lastStepAt_ += steps_.front().afterMs;
    state_[static_cast<size_t>(steps_.front().button)] = steps_.front().down;
    steps_.pop_front();
  }
  pressed = state_;
  return true;
}

}  // namespace wfw::hal
