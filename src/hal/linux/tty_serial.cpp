#include "hal/linux/tty_serial.h"

#include <fcntl.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>

#include "util/log.h"

namespace wfw::hal {

SerialLink::~SerialLink() { close(); }

void SerialLink::close() {
  if (fd_ >= 0) ::close(fd_);
  fd_ = -1;
  outbox_.clear();
  onClose();
}

bool SerialLink::readSome(std::string& out) {
  char buf[1024];
  for (;;) {
    const ssize_t n = ::read(fd_, buf, sizeof(buf));
    if (n > 0) {
      out.append(buf, static_cast<size_t>(n));
      continue;
    }
    if (n == 0) return false;  // hangup
    if (errno == EINTR) continue;
    if (errno == EAGAIN || errno == EWOULDBLOCK) return true;
    LOG_WARN("serial read: %s", std::strerror(errno));
    return false;
  }
}

void SerialLink::queue(const std::string& data) {
  // Drop the new line, never part of the queue: a partly written line must
  // be finished or the host would see two lines glued together.
  if (outbox_.size() + data.size() > kMaxQueuedBytes) {
    LOG_WARN("serial: host is not reading; dropping a %zu-byte reply", data.size());
    return;
  }
  outbox_ += data;
}

bool SerialLink::flush() {
  while (!outbox_.empty() && fd_ >= 0) {
    const ssize_t n = ::write(fd_, outbox_.data(), outbox_.size());
    if (n > 0) {
      outbox_.erase(0, static_cast<size_t>(n));
      continue;
    }
    if (n < 0 && errno == EINTR) continue;
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return true;
    LOG_WARN("serial write: %s", std::strerror(errno));
    return false;
  }
  return true;
}

namespace {

bool baudConstant(int baud, speed_t& out) {
  static const struct {
    int baud;
    speed_t constant;
  } kTable[] = {{9600, B9600},       {19200, B19200},     {38400, B38400},
                {57600, B57600},     {115200, B115200},   {230400, B230400},
                {460800, B460800},   {921600, B921600},   {1000000, B1000000},
                {2000000, B2000000}, {3000000, B3000000}, {4000000, B4000000}};
  for (const auto& e : kTable) {
    if (e.baud == baud) {
      out = e.constant;
      return true;
    }
  }
  return false;
}

bool makeRaw(int fd, speed_t speed) {
  termios tio{};
  if (tcgetattr(fd, &tio) != 0) return false;
  cfmakeraw(&tio);  // no echo, no line editing, no CR/LF translation
  tio.c_cflag |= CLOCAL | CREAD;
  tio.c_cflag &= ~static_cast<tcflag_t>(CRTSCTS);
  tio.c_cc[VMIN] = 0;
  tio.c_cc[VTIME] = 0;
  cfsetispeed(&tio, speed);
  cfsetospeed(&tio, speed);
  return tcsetattr(fd, TCSANOW, &tio) == 0;
}

}  // namespace

bool TtySerial::open() {
  speed_t speed = B115200;
  if (!baudConstant(baud_, speed)) LOG_WARN("serial: unsupported baud %d, using 115200", baud_);
  fd_ = ::open(path_.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
  if (fd_ < 0) {
    LOG_DEBUG("serial: open %s: %s", path_.c_str(), std::strerror(errno));
    return false;
  }
  if (!makeRaw(fd_, speed)) {
    LOG_ERROR("serial: cannot configure %s: %s", path_.c_str(), std::strerror(errno));
    close();
    return false;
  }
  // Refuse further opens (e.g. a stray getty) while we own the port.
  ::ioctl(fd_, TIOCEXCL);
  tcflush(fd_, TCIOFLUSH);
  LOG_INFO("serial: listening on %s", path_.c_str());
  return true;
}

PtySerial::~PtySerial() { onClose(); }

bool PtySerial::open() {
  fd_ = ::posix_openpt(O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
  if (fd_ < 0 || ::grantpt(fd_) != 0 || ::unlockpt(fd_) != 0) {
    LOG_ERROR("pty: %s", std::strerror(errno));
    close();
    return false;
  }
  const char* name = ::ptsname(fd_);
  slaveName_ = name ? name : "";
  keepAliveFd_ = ::open(slaveName_.c_str(), O_RDWR | O_NOCTTY | O_CLOEXEC);
  if (keepAliveFd_ < 0 || !makeRaw(keepAliveFd_, B115200)) {
    LOG_ERROR("pty: cannot open slave %s: %s", slaveName_.c_str(), std::strerror(errno));
    close();
    return false;
  }
  ::unlink(linkPath_.c_str());
  if (::symlink(slaveName_.c_str(), linkPath_.c_str()) != 0) {
    LOG_ERROR("pty: symlink %s: %s", linkPath_.c_str(), std::strerror(errno));
    close();
    return false;
  }
  LOG_INFO("serial (sim): open %s", describe().c_str());
  return true;
}

void PtySerial::onClose() {
  if (keepAliveFd_ >= 0) {
    ::close(keepAliveFd_);
    keepAliveFd_ = -1;
    ::unlink(linkPath_.c_str());
  }
}

}  // namespace wfw::hal
