// wallet-fw: firmware for the Aurora hardware wallet - a Raspberry Pi 4B with
// a Grove 16x2 RGB LCD, three Grove buttons and a Grove buzzer.
#include <fcntl.h>
#include <poll.h>
#include <sys/file.h>
#include <unistd.h>

#include <csignal>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>

#include "config.h"
#include "crypto/keys.h"
#include "device/controller.h"
#include "device/line_framer.h"
#include "device/protocol.h"
#include "hal/linux/sd_notify.h"
#include "hal/linux/tty_serial.h"
#include "hal/sim/sim_hal.h"
#include "util/fs.h"
#include "util/log.h"

#if defined(WFW_HARDWARE)
#include "hal/linux/gpiod_io.h"
#include "hal/linux/grove_lcd.h"
#include "hal/linux/i2c_bus.h"
#include "hw_test.h"
#endif

using namespace wfw;

namespace {

volatile std::sig_atomic_t g_stop = 0;
void onSignal(int) { g_stop = 1; }

constexpr const char* kDefaultConfig = "/etc/wallet-fw/config.json";

struct Options {
  std::string configPath = kDefaultConfig;
  bool configExplicit = false;
  std::string serial;
  std::string dataDir;
  bool sim = false;
  std::string ptyLink = "wallet-fw.pty";
  std::string simControl;
  std::string simDisplayLog;
  bool simQuiet = false;
  bool verbose = false;
  bool wipe = false;
  bool hwTest = false;
  std::string tcpListen;
};

void usage() {
  std::cout <<
      "Usage: wallet-fw [options]\n"
      "  --config PATH          config file (default " << kDefaultConfig << ")\n"
      "  --serial PATH          serial device (default /dev/ttyGS0; /dev/serial0 for the Pico)\n"
      "  --data-dir PATH        wallet storage directory (default /var/lib/wallet-fw)\n"
      "  --verbose              debug logging\n"
      "  --wipe                 delete the wallet and known recipients (asks to confirm)\n"
      "  --hw-test              LCD pages / buttons / buzzer check (stop the service first)\n"
      "  --tcp-listen HOST:PORT DEVELOPMENT ONLY: serve the protocol over TCP, not serial\n"
      "  --version              print the firmware version\n"
      "Simulator (no Pi hardware needed):\n"
      "  --sim                  console LCD, typed button commands, PTY serial port\n"
      "  --pty-link PATH        where to symlink the PTY (default ./wallet-fw.pty)\n"
      "  --sim-control PATH     read button commands from a file/FIFO instead of stdin\n"
      "  --sim-display-log PATH append each LCD frame and buzzer change as JSON lines\n"
      "  --sim-quiet            do not draw the LCD on stderr\n";
}

bool parseArgs(int argc, char** argv, Options& o) {
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto value = [&](std::string& out) {
      if (i + 1 >= argc) return false;
      out = argv[++i];
      return true;
    };
    bool ok = true;
    if (a == "--config") {
      ok = value(o.configPath);
      o.configExplicit = true;
    } else if (a == "--serial") ok = value(o.serial);
    else if (a == "--data-dir") ok = value(o.dataDir);
    else if (a == "--sim") o.sim = true;
    else if (a == "--pty-link") ok = value(o.ptyLink);
    else if (a == "--sim-control") ok = value(o.simControl);
    else if (a == "--sim-display-log") ok = value(o.simDisplayLog);
    else if (a == "--sim-quiet") o.simQuiet = true;
    else if (a == "--verbose") o.verbose = true;
    else if (a == "--wipe") o.wipe = true;
    else if (a == "--hw-test") o.hwTest = true;
    else if (a == "--tcp-listen") ok = value(o.tcpListen);
    else if (a == "--version") {
      std::cout << "Aurora firmware (wallet-fw) " << WFW_VERSION << " (protocol " << device::kProtocolVersion << ")\n";
      std::exit(0);
    } else if (a == "--help" || a == "-h") {
      usage();
      std::exit(0);
    } else {
      std::cerr << "unknown option: " << a << "\n";
      return false;
    }
    if (!ok) {
      std::cerr << "missing value for " << a << "\n";
      return false;
    }
  }
  return true;
}

int wipeWallet(const Config& cfg) {
  std::cout << "This permanently deletes the wallet in " << cfg.dataDir
            << ".\nFunds are only recoverable from the 12 words. Type WIPE to continue: "
            << std::flush;
  std::string answer;
  std::getline(std::cin, answer);
  if (answer != "WIPE") {
    std::cout << "Aborted.\n";
    return 1;
  }
  storage::Keystore ks(cfg.dataDir);
  storage::KnownRecipients rec(cfg.dataDir);
  std::string err;
  if (!ks.wipe(err) || !rec.clear(err)) {
    std::cerr << "wipe failed: " << err << "\n";
    return 1;
  }
  std::cout << "Wallet deleted.\n";
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  Options opt;
  if (!parseArgs(argc, argv, opt)) {
    usage();
    return 2;
  }
  if (opt.verbose) log::setLevel(log::Level::Debug);
#if !defined(WFW_HARDWARE)
  if (!opt.sim && !opt.wipe) {
    LOG_ERROR("built without hardware drivers (WFW_HARDWARE=OFF); use --sim");
    return 1;
  }
#endif
  if (!crypto::init()) {
    LOG_ERROR("libsodium failed to initialise");
    return 1;
  }

  Config cfg;
  if (opt.sim) cfg.dataDir = "sim-data";
  std::string err;
  if (!loadConfigFile(opt.configPath, cfg, !opt.configExplicit, err)) {
    LOG_ERROR("%s", err.c_str());
    return 1;
  }
  if (!opt.serial.empty()) cfg.serialDevice = opt.serial;
  if (!opt.dataDir.empty()) cfg.dataDir = opt.dataDir;
  if (opt.wipe) return wipeWallet(cfg);
#if defined(WFW_HARDWARE)
  if (opt.hwTest) {
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);
    return runHardwareTest(cfg, g_stop);
  }
#endif

  if (!fs::ensureDir(cfg.dataDir, 0700, err)) {
    LOG_ERROR("%s", err.c_str());
    return 1;
  }
  // One firmware instance per wallet: two would race on the keystore.
  const int lockFd = ::open(fs::joinPath(cfg.dataDir, ".lock").c_str(),
                            O_RDWR | O_CREAT | O_CLOEXEC, 0600);
  if (lockFd < 0 || ::flock(lockFd, LOCK_EX | LOCK_NB) != 0) {
    LOG_ERROR("another wallet-fw is already using %s", cfg.dataDir.c_str());
    return 1;
  }

  storage::Keystore keystore(cfg.dataDir);
  if (!keystore.load(err)) LOG_ERROR("keystore: %s", err.c_str());
  storage::DeviceIdentity identity(cfg.dataDir);
  if (!identity.loadOrCreate(err)) {
    LOG_ERROR("device identity: %s", err.c_str());
    return 1;
  }
  storage::KnownRecipients recipients(cfg.dataDir);
  if (!recipients.load(err)) LOG_WARN("recipients: %s (starting empty)", err.c_str());

  SteadyClock clock;
  std::unique_ptr<hal::Display> display;
  std::unique_ptr<hal::ButtonReader> buttonsHw;
  hal::Buzzer* buzzerHw = nullptr;
  std::unique_ptr<hal::Buzzer> simBuzzer;
  hal::SimButtons* simButtons = nullptr;
  std::unique_ptr<hal::SerialLink> link;
#if defined(WFW_HARDWARE)
  std::unique_ptr<hal::I2cBus> i2c;
#endif

  if (opt.sim) {
    int controlFd = STDIN_FILENO;
    if (!opt.simControl.empty()) {
      // O_RDWR keeps a writer open on a FIFO, so it never reports EOF
      // between test clients.
      controlFd = ::open(opt.simControl.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
      if (controlFd < 0) {
        LOG_ERROR("cannot open %s: %s", opt.simControl.c_str(), std::strerror(errno));
        return 1;
      }
    }
    display = std::make_unique<hal::SimDisplay>(clock, opt.simDisplayLog, !opt.simQuiet);
    simBuzzer = std::make_unique<hal::SimBuzzer>(clock, opt.simDisplayLog);
    buzzerHw = simBuzzer.get();
    auto sb = std::make_unique<hal::SimButtons>(clock, controlFd);
    simButtons = sb.get();
    buttonsHw = std::move(sb);
    if (opt.serial.empty()) {
      link = std::make_unique<hal::PtySerial>(opt.ptyLink);
    } else {
      link = std::make_unique<hal::TtySerial>(cfg.serialDevice, cfg.serialBaud);
    }
  } else {
#if defined(WFW_HARDWARE)
    i2c = std::make_unique<hal::I2cBus>(cfg.i2cBus);
    display = std::make_unique<hal::GroveLcd>(*i2c, cfg.lcdAddress, cfg.backlightAddress);
    hal::GpiodIo::Pins pins{cfg.gpioChip,
                            {static_cast<unsigned>(cfg.gpioLeft), static_cast<unsigned>(cfg.gpioMiddle),
                             static_cast<unsigned>(cfg.gpioRight)},
                            static_cast<unsigned>(cfg.gpioBuzzer),
                            cfg.buttonsActiveHigh};
    auto gpio = std::make_unique<hal::GpiodIo>(pins);
    if (!gpio->open(err)) {
      LOG_ERROR("%s", err.c_str());
      return 1;  // without buttons nothing can be approved; systemd retries
    }
    buzzerHw = gpio.get();
    buttonsHw = std::move(gpio);
    link = std::make_unique<hal::TtySerial>(cfg.serialDevice, cfg.serialBaud);
#else
    return 1;  // rejected above
#endif
  }

  if (!opt.tcpListen.empty()) {
    const auto colon = opt.tcpListen.rfind(':');
    const int port = colon == std::string::npos ? 0 : std::atoi(opt.tcpListen.c_str() + colon + 1);
    if (port <= 0 || port > 65535) {
      LOG_ERROR("--tcp-listen needs HOST:PORT, e.g. 127.0.0.1:7777");
      return 2;
    }
    link = std::make_unique<hal::TcpListenLink>(opt.tcpListen.substr(0, colon), port);
  }

  ui::ButtonInput buttons(cfg.debounceMs, cfg.holdConfirmMs);
  ui::ScreenRenderer screen(*display, clock, cfg.backlightBrightness);
  screen.begin();
  ui::BuzzerController buzzer(*buzzerHw, clock, cfg.buzzerEnabled);

  device::Controller controller(
      device::Services{keystore, recipients, identity, screen, buzzer, buttons, clock, cfg,
                       nullptr},
      [&link](const std::string& line) {
        if (!link->isOpen()) return;
        link->queue(line + "\n");
        link->flush();
      });
  controller.start();

  std::signal(SIGINT, onSignal);
  std::signal(SIGTERM, onSignal);
  std::signal(SIGPIPE, SIG_IGN);

  LOG_INFO("Aurora firmware %s started (%s, device %s, wallet %s)", WFW_VERSION,
           opt.sim ? "simulator" : "hardware", identity.deviceId().c_str(),
           keystore.initialized() ? keystore.address().c_str() : "not set up");
  hal::sdNotify("READY=1");
  const int64_t watchdogMs = hal::sdWatchdogIntervalMs();
  TimeMs lastWatchdog = 0;
  TimeMs nextOpenAttempt = 0;
  device::LineFramer framer(device::kMaxLineBytes);
  std::vector<ui::ButtonEvent> events;
  std::string inbound;

  while (!g_stop) {
    TimeMs now = clock.nowMs();
    if (!link->isOpen() && now >= nextOpenAttempt) {
      if (!link->open()) nextOpenAttempt = now + link->retryMs();
    }

    pollfd fds[2];
    nfds_t n = 0;
    int serialSlot = -1;
    int controlSlot = -1;
    if (link->isOpen()) {
      serialSlot = static_cast<int>(n);
      fds[n++] = {link->fd(), static_cast<short>(POLLIN | (link->wantsWrite() ? POLLOUT : 0)), 0};
    }
    if (simButtons && simButtons->fd() >= 0) {
      controlSlot = static_cast<int>(n);
      fds[n++] = {simButtons->fd(), POLLIN, 0};
    }
    // 5 ms: button sampling rate, well inside the 30 ms debounce window.
    if (::poll(fds, n, 5) < 0 && errno != EINTR) {
      LOG_ERROR("poll: %s", std::strerror(errno));
      break;
    }

    if (serialSlot >= 0) {
      const short re = fds[serialSlot].revents;
      bool healthy = true;
      if (re & POLLIN) {
        inbound.clear();
        healthy = link->readSome(inbound);
        device::LineFramer::Output out;
        framer.feed(inbound.data(), inbound.size(), out);
        for (size_t i = 0; i < out.overflows; ++i) controller.onOverflow();
        for (auto& line : out.lines) {
          controller.onLine(line);
          sodium_memzero(line.data(), line.size());  // may hold typed recovery words
        }
        sodium_memzero(inbound.data(), inbound.size());
      } else if (re & (POLLHUP | POLLERR | POLLNVAL)) {
        healthy = false;
      }
      if (healthy && (re & POLLOUT)) healthy = link->flush();
      if (!healthy) {
        LOG_WARN("serial link lost; reopening");
        link->close();
        framer.reset();
        controller.onLinkReset();
        nextOpenAttempt = clock.nowMs() + link->retryMs();
      }
    }
    if (controlSlot >= 0 && (fds[controlSlot].revents & (POLLIN | POLLHUP))) {
      simButtons->onReadable();
    }

    std::array<bool, 3> raw{};
    if (buttonsHw->read(raw)) {
      events.clear();
      buttons.update(raw, clock.nowMs(), events);
      for (const auto& ev : events) controller.onButton(ev);
    }
    controller.tick();

    now = clock.nowMs();
    if (watchdogMs > 0 && now - lastWatchdog >= watchdogMs / 2) {
      hal::sdNotify("WATCHDOG=1");
      lastWatchdog = now;
    }
  }

  LOG_INFO("shutting down");
  hal::sdNotify("STOPPING=1");
  buzzerHw->set(false);
  display->clear();
  display->setRGB(0, 0, 0);
  keystore.lock();
  return 0;
}
