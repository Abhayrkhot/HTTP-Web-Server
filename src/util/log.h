#pragma once

#include <string_view>

namespace hws::log {

enum class Level : int { Debug = 0, Info = 1, Warn = 2, Error = 3, Off = 4 };

void set_level(Level level) noexcept;
bool enabled(Level level) noexcept;
bool parse_level(std::string_view text, Level& out) noexcept;
// Short tag printed with every line from the calling thread, e.g. "w3".
void set_thread_tag(const char* tag) noexcept;

// Writes one line to stderr with a single write(2) so lines from different threads never interleave.
void write(Level level, const char* fmt, ...) noexcept __attribute__((format(printf, 2, 3)));

}  // namespace hws::log

#define HWS_LOG(level, ...)                                         \
  do {                                                              \
    if (::hws::log::enabled(level)) ::hws::log::write(level, __VA_ARGS__); \
  } while (0)
#define LOG_DEBUG(...) HWS_LOG(::hws::log::Level::Debug, __VA_ARGS__)
#define LOG_INFO(...) HWS_LOG(::hws::log::Level::Info, __VA_ARGS__)
#define LOG_WARN(...) HWS_LOG(::hws::log::Level::Warn, __VA_ARGS__)
#define LOG_ERROR(...) HWS_LOG(::hws::log::Level::Error, __VA_ARGS__)
