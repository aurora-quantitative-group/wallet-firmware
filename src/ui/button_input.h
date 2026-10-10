#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "util/clock.h"

namespace wfw::ui {

enum class Button : uint8_t { Left = 0, Middle = 1, Right = 2 };

struct ButtonEvent {
  enum class Kind : uint8_t { Press, Release, Hold };
  Kind kind;
  Button button;
  TimeMs at;         // when the event was generated
  TimeMs pressedAt;  // when the (debounced) press began
};

// Turns raw samples into debounced press/release events plus a single Hold
// event once a button has been down for `holdMs` (the 2 s confirm hold).
// A level must be stable for `debounceMs` (30 ms) before it is accepted.
class ButtonInput {
 public:
  ButtonInput(TimeMs debounceMs, TimeMs holdMs) : debounceMs_(debounceMs), holdMs_(holdMs) {}

  void update(const std::array<bool, 3>& raw, TimeMs now, std::vector<ButtonEvent>& out);

  bool pressed(Button b) const { return state(b).stable; }
  TimeMs pressedAt(Button b) const { return state(b).pressedAt; }
  TimeMs holdMs() const { return holdMs_; }

 private:
  struct State {
    bool stable = false;
    bool candidate = false;
    TimeMs candidateSince = 0;
    TimeMs pressedAt = 0;
    bool holdFired = false;
  };
  const State& state(Button b) const { return states_[static_cast<size_t>(b)]; }

  TimeMs debounceMs_;
  TimeMs holdMs_;
  std::array<State, 3> states_{};
};

}  // namespace wfw::ui
