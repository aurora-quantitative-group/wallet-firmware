#pragma once

#include <csignal>

#include "config.h"

namespace wfw {

// Hardware bring-up check, modelled on Seeed's rgb_lcd "pages" Arduino
// example: shows "Page 1" and steps through pages 1-3 on each middle press.
// Left/right presses are reported too, and right beeps the buzzer. Runs until
// `stop` is set (Ctrl+C). Stop the wallet-fw service first: it owns the GPIOs.
int runHardwareTest(const Config& cfg, volatile std::sig_atomic_t& stop);

}  // namespace wfw
