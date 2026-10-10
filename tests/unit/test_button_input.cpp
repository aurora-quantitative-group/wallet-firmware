#include "test.h"
#include "ui/button_input.h"
#include "ui/buzzer.h"
#include "util/clock.h"

using namespace wfw;
using namespace wfw::ui;

namespace {

struct Rig {
  ButtonInput input{30, 2000};
  TimeMs now = 1000;
  std::vector<ButtonEvent> events;

  // Holds the raw level for `ms`, sampling every 5 ms like the main loop.
  void level(std::array<bool, 3> raw, TimeMs ms) {
    for (TimeMs t = 0; t < ms; t += 5) {
      input.update(raw, now, events);
      now += 5;
    }
  }
  size_t count(ButtonEvent::Kind kind) const {
    size_t n = 0;
    for (const auto& e : events) n += e.kind == kind;
    return n;
  }
};

}  // namespace

TEST(button_bounce_shorter_than_debounce_is_ignored) {
  Rig r;
  for (int i = 0; i < 5; ++i) {
    r.level({true, false, false}, 10);
    r.level({false, false, false}, 10);
  }
  CHECK(r.events.empty());
}

TEST(button_press_and_release_after_debounce) {
  Rig r;
  r.level({false, true, false}, 100);
  REQUIRE(r.events.size() == 1);
  CHECK(r.events[0].kind == ButtonEvent::Kind::Press);
  CHECK(r.events[0].button == Button::Middle);
  CHECK(r.events[0].at - r.events[0].pressedAt >= 30);
  CHECK(r.input.pressed(Button::Middle));
  r.level({false, false, false}, 100);
  CHECK_EQ(r.count(ButtonEvent::Kind::Release), size_t{1});
  CHECK(!r.input.pressed(Button::Middle));
}

TEST(button_hold_fires_once_after_two_seconds) {
  Rig r;
  r.level({false, false, true}, 1900);
  CHECK_EQ(r.count(ButtonEvent::Kind::Hold), size_t{0});
  r.level({false, false, true}, 3000);
  CHECK_EQ(r.count(ButtonEvent::Kind::Hold), size_t{1});
  const auto& hold = r.events.back();
  CHECK(hold.kind == ButtonEvent::Kind::Hold);
  CHECK(hold.at - hold.pressedAt >= 2000);
  r.level({false, false, false}, 100);
  r.level({false, false, true}, 2100);
  CHECK_EQ(r.count(ButtonEvent::Kind::Hold), size_t{2});
}

TEST(button_short_release_during_hold_restarts_timer) {
  Rig r;
  r.level({false, false, true}, 1500);
  r.level({false, false, false}, 50);  // a real release
  r.level({false, false, true}, 1500);
  CHECK_EQ(r.count(ButtonEvent::Kind::Hold), size_t{0});
}

namespace {
struct FakeBuzzer : hal::Buzzer {
  std::vector<bool> changes;
  void set(bool on) override { changes.push_back(on); }
};
}  // namespace

TEST(buzzer_pattern_plays_and_stops) {
  ManualClock clock;
  FakeBuzzer hw;
  BuzzerController buzzer(hw, clock, true);
  buzzer.longTone();  // 80 on, 120 off, 1200 on
  CHECK(buzzer.sounding());
  clock.advance(100);
  buzzer.tick();
  CHECK(!buzzer.sounding());
  clock.advance(150);
  buzzer.tick();
  CHECK(buzzer.sounding());
  clock.advance(1300);
  buzzer.tick();
  CHECK(!buzzer.sounding());
  CHECK_EQ(hw.changes.size(), size_t{4});

  BuzzerController muted(hw, clock, false);
  muted.beep();
  CHECK(!muted.sounding());
}
