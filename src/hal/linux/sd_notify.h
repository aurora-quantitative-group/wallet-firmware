#pragma once

#include <cstdint>

namespace wfw::hal {

// Minimal sd_notify(3) without linking libsystemd: sends `state` (for
// example "READY=1" or "WATCHDOG=1") to $NOTIFY_SOCKET. No-op outside systemd.
void sdNotify(const char* state);

// Watchdog interval from $WATCHDOG_USEC in milliseconds, or 0 if disabled.
int64_t sdWatchdogIntervalMs();

}  // namespace wfw::hal
