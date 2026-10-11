#pragma once

#include <string>

namespace wfw::hal {

// Non-blocking byte link to the host app. Owns read/write buffering; the
// subclasses only know how to open their file descriptor.
class SerialLink {
 public:
  static constexpr size_t kMaxQueuedBytes = 256 * 1024;

  virtual ~SerialLink();
  virtual bool open() = 0;
  virtual std::string describe() const = 0;
  // How long to wait before retrying open() after it failed.
  virtual int retryMs() const { return 1000; }

  int fd() const { return fd_; }
  bool isOpen() const { return fd_ >= 0; }
  void close();
  // Appends everything currently readable. False on hangup or error: the
  // caller should close() and reopen later.
  bool readSome(std::string& out);
  void queue(const std::string& data);
  // Writes as much of the queue as the port accepts. False on error.
  bool flush();
  bool wantsWrite() const { return !outbox_.empty(); }

 protected:
  virtual void onClose() {}
  int fd_ = -1;

 private:
  std::string outbox_;
};

// A real tty: the USB gadget port /dev/ttyGS0 (Step 2) or the UART
// /dev/serial0 used by the Pico fallback. Raw mode, no echo, given baud.
class TtySerial final : public SerialLink {
 public:
  TtySerial(std::string path, int baud) : path_(std::move(path)), baud_(baud) {}
  bool open() override;
  std::string describe() const override { return path_; }

 private:
  std::string path_;
  int baud_;
};

// Simulator: a pseudo-terminal whose slave side is symlinked at `linkPath`,
// so a host tool (or the app via socat) can open it like a serial port.
class PtySerial final : public SerialLink {
 public:
  explicit PtySerial(std::string linkPath) : linkPath_(std::move(linkPath)) {}
  ~PtySerial() override;
  bool open() override;
  std::string describe() const override { return linkPath_ + " -> " + slaveName_; }

 protected:
  void onClose() override;

 private:
  std::string linkPath_;
  std::string slaveName_;
  int keepAliveFd_ = -1;  // our own slave fd keeps the master from seeing HUP
};

// DEVELOPMENT ONLY: serves the protocol to one TCP client at a time, so the
// Aurora app (debug build, WALLET_SERIAL_PORT=tcp://host:port) can reach the
// simulator or a Pi without a USB cable. Anyone who can reach the port can
// send requests; approving anything still needs the physical buttons.
class TcpListenLink final : public SerialLink {
 public:
  TcpListenLink(std::string host, int port) : host_(std::move(host)), port_(port) {}
  ~TcpListenLink() override;
  bool open() override;  // accepts a waiting client, if any
  std::string describe() const override { return "tcp://" + host_ + ":" + std::to_string(port_); }
  int retryMs() const override { return listenFailing_ ? 2000 : 100; }

 private:
  std::string host_;
  int port_;
  int listenFd_ = -1;
  bool listenFailing_ = false;
};

}  // namespace wfw::hal
