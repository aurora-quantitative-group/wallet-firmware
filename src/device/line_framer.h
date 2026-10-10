#pragma once

#include <string>
#include <vector>

namespace wfw::device {

// Splits a byte stream into newline-terminated lines (build manual Step 4:
// one JSON object per line). A trailing '\r' is stripped. A line longer than
// maxLine is discarded up to its newline and counted as one overflow, so a
// runaway sender cannot exhaust memory or desynchronise framing.
class LineFramer {
 public:
  explicit LineFramer(size_t maxLine) : maxLine_(maxLine) {}

  struct Output {
    std::vector<std::string> lines;
    size_t overflows = 0;
  };

  void feed(const char* data, size_t len, Output& out);
  void reset();

 private:
  size_t maxLine_;
  std::string buffer_;
  bool discarding_ = false;
};

}  // namespace wfw::device
