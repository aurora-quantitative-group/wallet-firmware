#pragma once
// Minimal self-contained test harness (no external framework to download).

#include <cstdint>
#include <sstream>
#include <string>
#include <type_traits>
#include <vector>

namespace t {

struct Case {
  const char* name;
  void (*fn)();
};
std::vector<Case>& registry();
struct Reg {
  Reg(const char* name, void (*fn)()) { registry().push_back({name, fn}); }
};
struct Abort {};

void fail(const char* file, int line, const std::string& message);

template <typename T, typename = void>
struct Streamable : std::false_type {};
template <typename T>
struct Streamable<T, std::void_t<decltype(std::declval<std::ostream&>() << std::declval<T>())>>
    : std::true_type {};

template <typename T>
std::string show(const T& v) {
  if constexpr (std::is_same_v<T, uint8_t>) {
    return std::to_string(static_cast<int>(v));
  } else if constexpr (std::is_same_v<T, bool>) {
    return v ? "true" : "false";
  } else if constexpr (Streamable<T>::value) {
    std::ostringstream s;
    s << v;
    return s.str();
  } else {
    return "<unprintable>";
  }
}

}  // namespace t

#define TEST(name)                                   \
  static void name();                                \
  static const ::t::Reg reg_##name(#name, &(name));  \
  static void name()

#define CHECK(cond)                                                     \
  do {                                                                  \
    if (!(cond)) ::t::fail(__FILE__, __LINE__, "CHECK(" #cond ")");     \
  } while (0)

#define REQUIRE(cond)                                                   \
  do {                                                                  \
    if (!(cond)) {                                                      \
      ::t::fail(__FILE__, __LINE__, "REQUIRE(" #cond ")");              \
      throw ::t::Abort{};                                               \
    }                                                                   \
  } while (0)

#define CHECK_EQ(a, b)                                                                   \
  do {                                                                                   \
    /* copies: (a) may be a reference into a temporary, e.g. send(...)["k"] */          \
    const auto _a = (a);                                                                 \
    const auto _b = (b);                                                                 \
    if (!(_a == _b)) {                                                                   \
      ::t::fail(__FILE__, __LINE__,                                                      \
                "CHECK_EQ(" #a ", " #b "): " + ::t::show(_a) + " != " + ::t::show(_b)); \
    }                                                                                    \
  } while (0)
