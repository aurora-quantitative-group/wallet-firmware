#include "device/line_framer.h"
#include "test.h"

using wfw::device::LineFramer;

namespace {
void feed(LineFramer& f, const std::string& s, LineFramer::Output& out) {
  f.feed(s.data(), s.size(), out);
}
}  // namespace

TEST(framer_splits_lines_across_chunks) {
  LineFramer f(64);
  LineFramer::Output out;
  feed(f, "{\"cmd\":\"pi", out);
  CHECK(out.lines.empty());
  feed(f, "ng\"}\r\n{\"cmd\":\"get_pubkey\"}\n\n\r\n{\"a\"", out);
  REQUIRE(out.lines.size() == 2);
  CHECK_EQ(out.lines[0], std::string("{\"cmd\":\"ping\"}"));
  CHECK_EQ(out.lines[1], std::string("{\"cmd\":\"get_pubkey\"}"));
  feed(f, ":1}\n", out);
  REQUIRE(out.lines.size() == 3);
  CHECK_EQ(out.lines[2], std::string("{\"a\":1}"));
  CHECK_EQ(out.overflows, size_t{0});
}

TEST(framer_discards_oversized_line_then_recovers) {
  LineFramer f(8);
  LineFramer::Output out;
  feed(f, "0123456789abcdef", out);
  CHECK_EQ(out.overflows, size_t{1});
  feed(f, "more junk", out);
  CHECK_EQ(out.overflows, size_t{1});
  feed(f, "\nok\n", out);
  REQUIRE(out.lines.size() == 1);
  CHECK_EQ(out.lines[0], std::string("ok"));
  f.reset();
  feed(f, "x\n", out);
  CHECK_EQ(out.lines.back(), std::string("x"));
}
