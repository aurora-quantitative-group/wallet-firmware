#pragma once

#include <cstdio>
#include <deque>
#include <string>

#include "hal/interfaces.h"
#include "util/clock.h"

namespace wfw::hal {

// Simulator HAL: runs the real firmware on any Linux box without the Pi.
// The display is drawn on stderr and (optionally) appended as JSON lines to
// a log file that integration tests read; buttons come from text commands.

class SimDisplay final : public Display {
 public:
  SimDisplay(const Clock& clock, std::string logPath, bool drawConsole);
  ~SimDisplay() override;

  bool init() override { return true; }
  bool clear() override;
  bool print(int row, const std::string& text) override;
  bool setRGB(uint8_t r, uint8_t g, uint8_t b) override;

 private:
  void emit();

  const Clock& clock_;
  FILE* log_ = nullptr;
  bool drawConsole_;
  std::string rows_[2];
  uint8_t rgb_[3] = {0, 0, 0};
};

class SimBuzzer final : public Buzzer {
 public:
  SimBuzzer(const Clock& clock, std::string logPath);
  ~SimBuzzer() override;
  void set(bool on) override;

 private:
  const Clock& clock_;
  FILE* log_ = nullptr;
};

// Button commands, one per line (stdin or a FIFO):
//   l | m | r            click left / middle / right (100 ms press)
//   h                    hold right for 2500 ms (confirm)
//   click <l|m|r> [ms]   press for ms (default 100)
//   hold <l|m|r> [ms]    press for ms (default 2500)
//   press <l|m|r> / release <l|m|r>
// Commands queue up and play back in order.
class SimButtons final : public ButtonReader {
 public:
  SimButtons(const Clock& clock, int fd) : clock_(clock), fd_(fd) {}

  int fd() const { return fd_; }
  // Reads pending command text; closes the input on EOF.
  void onReadable();
  void command(const std::string& line);
  bool read(std::array<bool, 3>& pressed) override;

 private:
  struct Step {
    int button;
    bool down;
    TimeMs afterMs;  // delay after the previous step
  };

  const Clock& clock_;
  int fd_;
  std::string partial_;
  std::deque<Step> steps_;
  TimeMs lastStepAt_ = 0;
  std::array<bool, 3> state_{};
};

}  // namespace wfw::hal
