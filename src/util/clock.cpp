#include "util/clock.h"

#include <chrono>

namespace wfw {

TimeMs SteadyClock::nowMs() const {
  using namespace std::chrono;
  return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

}  // namespace wfw
