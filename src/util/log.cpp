#include "util/log.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>

namespace wfw::log {
namespace {

Level g_level = Level::Info;

bool underJournal() {
  static const bool journal = std::getenv("JOURNAL_STREAM") != nullptr;
  return journal;
}

}  // namespace

void setLevel(Level level) { g_level = level; }
Level level() { return g_level; }

void write(Level lvl, const char* fmt, ...) {
  if (lvl < g_level) return;
  static const char* const kTags[] = {"DEBUG", "INFO", "WARN", "ERROR"};
  static const char* const kPriorities[] = {"<7>", "<6>", "<4>", "<3>"};
  const int i = static_cast<int>(lvl);

  char msg[1024];
  va_list args;
  va_start(args, fmt);
  std::vsnprintf(msg, sizeof(msg), fmt, args);
  va_end(args);

  if (underJournal()) {
    std::fprintf(stderr, "%s%s\n", kPriorities[i], msg);
  } else {
    std::fprintf(stderr, "[%s] %s\n", kTags[i], msg);
  }
}

}  // namespace wfw::log
