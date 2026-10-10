#pragma once

#include <cstdint>

namespace wfw {

using TimeMs = int64_t;

// Monotonic millisecond clock. Injected everywhere time matters so the state
// machines can be driven deterministically from tests.
class Clock {
 public:
  virtual ~Clock() = default;
  virtual TimeMs nowMs() const = 0;
};

class SteadyClock final : public Clock {
 public:
  TimeMs nowMs() const override;
};

class ManualClock final : public Clock {
 public:
  explicit ManualClock(TimeMs start = 1000) : now_(start) {}
  TimeMs nowMs() const override { return now_; }
  void advance(TimeMs ms) { now_ += ms; }
  void set(TimeMs ms) { now_ = ms; }

 private:
  TimeMs now_;
};

}  // namespace wfw
