#pragma once

namespace wfw::log {

enum class Level { Debug = 0, Info = 1, Warn = 2, Error = 3 };

void setLevel(Level level);
Level level();

// printf-style logging to stderr. Under systemd (JOURNAL_STREAM set) each line
// gets a syslog priority prefix so journald records the right severity.
void write(Level level, const char* fmt, ...) __attribute__((format(printf, 2, 3)));

}  // namespace wfw::log

#define LOG_DEBUG(...) ::wfw::log::write(::wfw::log::Level::Debug, __VA_ARGS__)
#define LOG_INFO(...) ::wfw::log::write(::wfw::log::Level::Info, __VA_ARGS__)
#define LOG_WARN(...) ::wfw::log::write(::wfw::log::Level::Warn, __VA_ARGS__)
#define LOG_ERROR(...) ::wfw::log::write(::wfw::log::Level::Error, __VA_ARGS__)
