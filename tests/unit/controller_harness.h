#pragma once
// Drives the real Controller with fake hardware and a manual clock, at the
// same 5 ms sampling cadence as the firmware's main loop.

#include <sodium.h>

#include <memory>
#include <nlohmann/json.hpp>

#include "config.h"
#include "device/controller.h"
#include "test_util.h"
#include "test_vectors.h"

namespace test {

using nlohmann::json;
using wfw::ui::Button;

struct FakeDisplay : wfw::hal::Display {
  std::string rows[2];
  uint8_t rgb[3] = {0, 0, 0};
  bool broken = false;
  bool init() override { return !broken; }
  bool clear() override {
    rows[0] = rows[1] = "";
    return !broken;
  }
  bool print(int row, const std::string& text) override {
    rows[row] = text;
    return !broken;
  }
  bool setRGB(uint8_t r, uint8_t g, uint8_t b) override {
    rgb[0] = r;
    rgb[1] = g;
    rgb[2] = b;
    return !broken;
  }
};

struct FakeBuzzer : wfw::hal::Buzzer {
  int onCount = 0;
  bool on = false;
  void set(bool v) override {
    on = v;
    onCount += v;
  }
};

struct Harness {
  TempDir dir;
  wfw::Config cfg;
  wfw::ManualClock clock;
  FakeDisplay display;
  FakeBuzzer buzzerHw;
  std::unique_ptr<wfw::storage::Keystore> keystore;
  std::unique_ptr<wfw::storage::KnownRecipients> recipients;
  std::unique_ptr<wfw::ui::ButtonInput> buttons;
  std::unique_ptr<wfw::ui::ScreenRenderer> screen;
  std::unique_ptr<wfw::ui::BuzzerController> buzzer;
  std::unique_ptr<wfw::device::Controller> ctrl;
  std::vector<json> out;
  std::array<bool, 3> raw{};
  uint8_t entropyFill = 0;  // new wallets: 0 -> "abandon ... about"

  // Creates the wallet on disk before boot (skipping the setup prompt).
  void preinit(uint8_t fill = 0, const std::string& pin = "") {
    wfw::storage::Keystore ks(dir.path);
    ks.setKdfLimits(crypto_pwhash_OPSLIMIT_MIN, crypto_pwhash_MEMLIMIT_MIN);
    std::string err;
    ks.load(err);
    uint8_t entropy[16];
    std::fill(std::begin(entropy), std::end(entropy), fill);
    ks.create(entropy, pin, err);
  }

  // PIN digits are dialled with right (+1) and committed with middle.
  void enterPin(const std::string& pin) {
    for (const char c : pin) {
      clicks(Button::Right, c - '0');
      click(Button::Middle);
    }
  }

  void boot() {
    std::string err;
    keystore = std::make_unique<wfw::storage::Keystore>(dir.path);
    keystore->setKdfLimits(crypto_pwhash_OPSLIMIT_MIN, crypto_pwhash_MEMLIMIT_MIN);
    keystore->load(err);
    recipients = std::make_unique<wfw::storage::KnownRecipients>(dir.path);
    recipients->load(err);
    buttons = std::make_unique<wfw::ui::ButtonInput>(cfg.debounceMs, cfg.holdConfirmMs);
    screen = std::make_unique<wfw::ui::ScreenRenderer>(display, clock, 100);
    screen->begin();
    buzzer = std::make_unique<wfw::ui::BuzzerController>(buzzerHw, clock, true);
    const uint8_t fill = entropyFill;
    ctrl = std::make_unique<wfw::device::Controller>(
        wfw::device::Services{*keystore, *recipients, *screen, *buzzer, *buttons, clock, cfg,
                              [fill](uint8_t* b, size_t n) { std::fill(b, b + n, fill); }},
        [this](const std::string& line) { out.push_back(json::parse(line)); });
    ctrl->start();
    out.clear();
  }

  // Sends a request; returns the reply if one arrived immediately, else null.
  json send(const json& req) { return sendRaw(req.dump()); }
  json sendRaw(const std::string& line) {
    const size_t before = out.size();
    ctrl->onLine(line);
    return out.size() > before ? out.back() : json();
  }
  // Runs the loop for `ms`; returns the newest reply produced meanwhile.
  json run(wfw::TimeMs ms) {
    const size_t before = out.size();
    std::vector<wfw::ui::ButtonEvent> events;
    for (wfw::TimeMs t = 0; t < ms; t += 5) {
      events.clear();
      buttons->update(raw, clock.nowMs(), events);
      for (const auto& ev : events) ctrl->onButton(ev);
      ctrl->tick();
      clock.advance(5);
    }
    return out.size() > before ? out.back() : json();
  }
  json click(Button b) {
    raw[static_cast<size_t>(b)] = true;
    json a = run(80);
    raw[static_cast<size_t>(b)] = false;
    json r = run(60);
    return r.is_null() ? a : r;
  }
  json hold(Button b, wfw::TimeMs ms = 2200) {
    raw[static_cast<size_t>(b)] = true;
    json a = run(ms);
    raw[static_cast<size_t>(b)] = false;
    json r = run(60);
    return r.is_null() ? a : r;
  }
  void clicks(Button b, int n) {
    for (int i = 0; i < n; ++i) click(b);
  }

  static std::string trim(std::string s) {
    while (!s.empty() && s.back() == ' ') s.pop_back();
    return s;
  }
  std::string top() const { return trim(display.rows[0]); }
  std::string bottom() const { return trim(display.rows[1]); }
  bool colorIs(wfw::ui::Rgb c) const {
    return display.rgb[0] == c.r && display.rgb[1] == c.g && display.rgb[2] == c.b;
  }

  // Walks the 12 words (reading them off the LCD) and confirms on word 12.
  std::string readWordsAndConfirm() {
    click(Button::Middle);
    std::string words;
    for (int i = 0; i < 12; ++i) {
      const std::string t = top();  // "03/12 ocean"
      words += (i ? " " : "") + t.substr(t.find(' ') + 1);
      if (i < 11) click(Button::Middle);
    }
    hold(Button::Right);
    return words;
  }
};

inline std::string b64msg(const char* hexMsg) {
  return wfw::base64Encode(*wfw::hexDecode(hexMsg));
}

}  // namespace test
