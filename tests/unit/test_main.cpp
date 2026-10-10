#include <cstdio>
#include <cstring>
#include <exception>

#include "crypto/keys.h"
#include "test.h"
#include "util/log.h"

namespace t {

std::vector<Case>& registry() {
  static std::vector<Case> cases;
  return cases;
}

namespace {
int g_failures = 0;
const char* g_current = "";
}  // namespace

void fail(const char* file, int line, const std::string& message) {
  ++g_failures;
  std::fprintf(stderr, "FAIL %s (%s:%d): %s\n", g_current, file, line, message.c_str());
}

}  // namespace t

// Usage: wfw_tests [substring]   runs every test whose name contains it.
int main(int argc, char** argv) {
  if (!wfw::crypto::init()) return 1;
  wfw::log::setLevel(wfw::log::Level::Error);
  const char* filter = argc > 1 ? argv[1] : nullptr;
  int run = 0;
  int failedTests = 0;
  for (const auto& c : t::registry()) {
    if (filter && std::strstr(c.name, filter) == nullptr) continue;
    t::g_current = c.name;
    const int before = t::g_failures;
    try {
      c.fn();
    } catch (const t::Abort&) {
    } catch (const std::exception& e) {
      t::fail(__FILE__, __LINE__, std::string("uncaught exception: ") + e.what());
    }
    ++run;
    if (t::g_failures != before) ++failedTests;
  }
  std::printf("%d tests, %d failed, %d failed checks\n", run, failedTests, t::g_failures);
  return failedTests == 0 && run > 0 ? 0 : 1;
}
