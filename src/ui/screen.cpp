#include "ui/screen.h"

#include "solana/format.h"
#include "util/log.h"

namespace wfw::ui {

ScreenRenderer::ScreenRenderer(hal::Display& display, const Clock& clock, int brightnessPercent)
    : display_(display), clock_(clock), brightness_(brightnessPercent) {}

bool ScreenRenderer::begin() {
  lastAttempt_ = clock_.nowMs();
  healthy_ = display_.init();
  if (!healthy_) {
    LOG_ERROR("display did not respond; signing is disabled until it does");
    return false;
  }
  return draw(true);
}

void ScreenRenderer::show(const Screen& screen) {
  wanted_ = Screen{lcdLine(screen.top), lcdLine(screen.bottom), screen.color};
  if (healthy_) draw(false);
}

void ScreenRenderer::retryIfNeeded() {
  if (healthy_) return;
  const TimeMs now = clock_.nowMs();
  if (now - lastAttempt_ < 1000) return;
  lastAttempt_ = now;
  if (display_.init()) {
    healthy_ = true;
    LOG_INFO("display recovered");
    draw(true);
  }
}

bool ScreenRenderer::draw(bool full) {
  bool ok = true;
  if (full || !everDrawn_) {
    ok = display_.clear();
    drawn_ = Screen{std::string(kLcdCols, ' '), std::string(kLcdCols, ' '), colors::kOff};
    full = true;
  }
  if (ok && (full || wanted_.top != drawn_.top)) ok = display_.print(0, wanted_.top);
  if (ok && (full || wanted_.bottom != drawn_.bottom)) ok = display_.print(1, wanted_.bottom);
  if (ok && (full || wanted_.color != drawn_.color)) {
    auto scale = [this](uint8_t v) { return static_cast<uint8_t>(v * brightness_ / 100); };
    ok = display_.setRGB(scale(wanted_.color.r), scale(wanted_.color.g), scale(wanted_.color.b));
  }
  if (!ok) {
    if (healthy_) LOG_ERROR("display write failed; will retry");
    healthy_ = false;
    lastAttempt_ = clock_.nowMs();
    return false;
  }
  drawn_ = wanted_;
  everDrawn_ = true;
  return true;
}

}  // namespace wfw::ui
