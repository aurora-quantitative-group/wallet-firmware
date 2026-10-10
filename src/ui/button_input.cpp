#include "ui/button_input.h"

namespace wfw::ui {

void ButtonInput::update(const std::array<bool, 3>& raw, TimeMs now,
                         std::vector<ButtonEvent>& out) {
  for (size_t i = 0; i < states_.size(); ++i) {
    State& s = states_[i];
    const auto button = static_cast<Button>(i);

    if (raw[i] != s.candidate) {
      s.candidate = raw[i];
      s.candidateSince = now;
    }
    if (s.candidate != s.stable && now - s.candidateSince >= debounceMs_) {
      s.stable = s.candidate;
      if (s.stable) {
        s.pressedAt = s.candidateSince;
        s.holdFired = false;
        out.push_back({ButtonEvent::Kind::Press, button, now, s.pressedAt});
      } else {
        out.push_back({ButtonEvent::Kind::Release, button, now, s.pressedAt});
      }
    }
    if (s.stable && !s.holdFired && now - s.pressedAt >= holdMs_) {
      s.holdFired = true;
      out.push_back({ButtonEvent::Kind::Hold, button, now, s.pressedAt});
    }
  }
}

}  // namespace wfw::ui
