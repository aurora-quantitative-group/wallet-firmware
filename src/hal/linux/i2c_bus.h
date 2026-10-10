#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace wfw::hal {

// /dev/i2c-N via the i2c-dev ioctl interface. Each transfer names its own
// slave address (I2C_RDWR), so one bus object serves several chips.
class I2cBus {
 public:
  explicit I2cBus(std::string path) : path_(std::move(path)) {}
  ~I2cBus();
  I2cBus(const I2cBus&) = delete;
  I2cBus& operator=(const I2cBus&) = delete;

  bool open();
  bool isOpen() const { return fd_ >= 0; }
  bool write(uint8_t address, const uint8_t* data, size_t len);
  // True if a chip ACKs a one-byte read at this address.
  bool probe(uint8_t address);
  const std::string& path() const { return path_; }

 private:
  std::string path_;
  int fd_ = -1;
};

}  // namespace wfw::hal
