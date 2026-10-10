#pragma once

#include <deque>

#include "hal/interfaces.h"
#include "util/clock.h"

namespace wfw::ui {

// Non-blocking buzzer patterns driven from the main loop's tick().
class BuzzerController {
 public:
  static constexpr TimeMs kShortBeepMs = 80;   // signing request
  static constexpr TimeMs kLongToneMs = 1200;  // high-risk transaction

  BuzzerController(hal::Buzzer& buzzer, const Clock& clock, bool enabled)
      : buzzer_(buzzer), clock_(clock), enabled_(enabled) {}

  void beep() { play({kShortBeepMs}); }
  void longTone() { play({kShortBeepMs, 120, kLongToneMs}); }  // beep, gap, tone
  // Alternating on/off durations, starting with "on". Replaces any pattern.
  void play(std::initializer_list<TimeMs> pattern);
  void stop();
  void tick();
  bool sounding() const { return on_; }

 private:
  void apply(bool on);

  hal::Buzzer& buzzer_;
  const Clock& clock_;
  bool enabled_;
  std::deque<TimeMs> segments_;
  bool on_ = false;
  bool nextOn_ = true;
  TimeMs segmentEnd_ = 0;
};

}  // namespace wfw::ui
