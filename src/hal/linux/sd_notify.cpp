#include "hal/linux/sd_notify.h"

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cstdlib>
#include <cstring>

namespace wfw::hal {

void sdNotify(const char* state) {
  const char* path = std::getenv("NOTIFY_SOCKET");
  if (path == nullptr || path[0] == '\0') return;
  const size_t len = std::strlen(path);
  sockaddr_un addr{};
  if (len >= sizeof(addr.sun_path)) return;
  addr.sun_family = AF_UNIX;
  std::memcpy(addr.sun_path, path, len);
  if (addr.sun_path[0] == '@') addr.sun_path[0] = '\0';  // abstract namespace

  const int fd = ::socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
  if (fd < 0) return;
  ::sendto(fd, state, std::strlen(state), MSG_NOSIGNAL, reinterpret_cast<sockaddr*>(&addr),
           static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + len));
  ::close(fd);
}

int64_t sdWatchdogIntervalMs() {
  const char* usec = std::getenv("WATCHDOG_USEC");
  if (usec == nullptr) return 0;
  const long long v = std::atoll(usec);
  return v > 0 ? v / 1000 : 0;
}

}  // namespace wfw::hal
