#include "ui/buzzer.h"

namespace wfw::ui {

void BuzzerController::play(std::initializer_list<TimeMs> pattern) {
  if (!enabled_) return;
  segments_.assign(pattern.begin(), pattern.end());
  nextOn_ = true;
  segmentEnd_ = clock_.nowMs();
  tick();
}

void BuzzerController::stop() {
  segments_.clear();
  apply(false);
}

void BuzzerController::tick() {
  const TimeMs now = clock_.nowMs();
  while (now >= segmentEnd_) {
    if (segments_.empty()) {
      apply(false);
      return;
    }
    segmentEnd_ += segments_.front();
    segments_.pop_front();
    apply(nextOn_);
    nextOn_ = !nextOn_;
  }
}

void BuzzerController::apply(bool on) {
  if (on == on_) return;
  on_ = on;
  buzzer_.set(on);
}

}  // namespace wfw::ui
