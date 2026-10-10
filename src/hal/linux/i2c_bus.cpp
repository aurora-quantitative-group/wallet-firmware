#include "hal/linux/i2c_bus.h"

#include <fcntl.h>
#include <linux/i2c-dev.h>
#include <linux/i2c.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

#include "util/log.h"

namespace wfw::hal {

I2cBus::~I2cBus() {
  if (fd_ >= 0) ::close(fd_);
}

bool I2cBus::open() {
  if (fd_ >= 0) return true;
  fd_ = ::open(path_.c_str(), O_RDWR | O_CLOEXEC);
  if (fd_ < 0) {
    LOG_ERROR("i2c: open %s: %s (is I2C enabled in raspi-config?)", path_.c_str(),
              std::strerror(errno));
    return false;
  }
  return true;
}

bool I2cBus::write(uint8_t address, const uint8_t* data, size_t len) {
  if (fd_ < 0) return false;
  i2c_msg msg{};
  msg.addr = address;
  msg.flags = 0;
  msg.len = static_cast<__u16>(len);
  msg.buf = const_cast<uint8_t*>(data);
  i2c_rdwr_ioctl_data xfer{&msg, 1};
  for (int attempt = 0; attempt < 3; ++attempt) {
    if (::ioctl(fd_, I2C_RDWR, &xfer) == 1) return true;
    if (errno != EAGAIN && errno != EREMOTEIO && errno != EIO) break;
    ::usleep(1000);
  }
  LOG_DEBUG("i2c: write to 0x%02x failed: %s", address, std::strerror(errno));
  return false;
}

bool I2cBus::probe(uint8_t address) {
  if (fd_ < 0) return false;
  uint8_t byte = 0;
  i2c_msg msg{};
  msg.addr = address;
  msg.flags = I2C_M_RD;
  msg.len = 1;
  msg.buf = &byte;
  i2c_rdwr_ioctl_data xfer{&msg, 1};
  return ::ioctl(fd_, I2C_RDWR, &xfer) == 1;
}

}  // namespace wfw::hal
