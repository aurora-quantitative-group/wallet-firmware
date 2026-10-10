#pragma once

#include <sodium.h>

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace wfw::crypto {

// Fixed-size buffer for key material. Non-copyable; wiped with
// sodium_memzero on destruction so secrets do not linger on the stack/heap.
template <size_t N>
class Secret {
 public:
  Secret() { std::memset(bytes_, 0, N); }
  ~Secret() { sodium_memzero(bytes_, N); }
  Secret(const Secret&) = delete;
  Secret& operator=(const Secret&) = delete;

  uint8_t* data() { return bytes_; }
  const uint8_t* data() const { return bytes_; }
  static constexpr size_t size() { return N; }
  uint8_t& operator[](size_t i) { return bytes_[i]; }
  const uint8_t& operator[](size_t i) const { return bytes_[i]; }

  void copyFrom(const Secret& other) { std::memcpy(bytes_, other.bytes_, N); }
  void copyFrom(const uint8_t* src) { std::memcpy(bytes_, src, N); }
  void wipe() { sodium_memzero(bytes_, N); }

 private:
  uint8_t bytes_[N];
};

}  // namespace wfw::crypto
