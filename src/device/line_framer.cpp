#include "device/line_framer.h"

namespace wfw::device {

void LineFramer::feed(const char* data, size_t len, Output& out) {
  for (size_t i = 0; i < len; ++i) {
    const char c = data[i];
    if (c == '\n') {
      if (discarding_) {
        discarding_ = false;
      } else {
        if (!buffer_.empty() && buffer_.back() == '\r') buffer_.pop_back();
        if (!buffer_.empty()) out.lines.push_back(std::move(buffer_));
      }
      buffer_.clear();
      continue;
    }
    if (discarding_) continue;
    if (buffer_.size() >= maxLine_) {
      buffer_.clear();
      buffer_.shrink_to_fit();
      discarding_ = true;
      ++out.overflows;
      continue;
    }
    buffer_.push_back(c);
  }
}

void LineFramer::reset() {
  buffer_.clear();
  discarding_ = false;
}

}  // namespace wfw::device
