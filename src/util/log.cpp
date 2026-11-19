#include "util/log.h"

#include <unistd.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <ctime>

#include "util/strings.h"

namespace hws::log {
namespace {

std::atomic<int> g_level{static_cast<int>(Level::Info)};
thread_local char t_tag[16] = "main";

const char* level_name(Level level) noexcept {
  switch (level) {
    case Level::Debug: return "DEBUG";
    case Level::Info: return "INFO";
    case Level::Warn: return "WARN";
    case Level::Error: return "ERROR";
    case Level::Off: break;
  }
  return "OFF";
}

}  // namespace

void set_level(Level level) noexcept { g_level.store(static_cast<int>(level), std::memory_order_relaxed); }

bool enabled(Level level) noexcept {
  return static_cast<int>(level) >= g_level.load(std::memory_order_relaxed);
}

bool parse_level(std::string_view text, Level& out) noexcept {
  if (iequals(text, "debug")) out = Level::Debug;
  else if (iequals(text, "info")) out = Level::Info;
  else if (iequals(text, "warn") || iequals(text, "warning")) out = Level::Warn;
  else if (iequals(text, "error")) out = Level::Error;
  else if (iequals(text, "off")) out = Level::Off;
  else return false;
  return true;
}

void set_thread_tag(const char* tag) noexcept { std::snprintf(t_tag, sizeof(t_tag), "%s", tag); }

void write(Level level, const char* fmt, ...) noexcept {
  char buf[2048];
  timespec ts{};
  clock_gettime(CLOCK_REALTIME, &ts);
  tm t{};
  gmtime_r(&ts.tv_sec, &t);
  int n = std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d.%03ldZ %-5s [%s] ", t.tm_year + 1900,
                        t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec, ts.tv_nsec / 1000000L,
                        level_name(level), t_tag);
  if (n < 0) return;
  size_t len = static_cast<size_t>(n);
  va_list ap;
  va_start(ap, fmt);
  const int m = std::vsnprintf(buf + len, sizeof(buf) - len - 1, fmt, ap);
  va_end(ap);
  if (m > 0) len += static_cast<size_t>(m) < sizeof(buf) - len - 1 ? static_cast<size_t>(m) : sizeof(buf) - len - 2;
  buf[len++] = '\n';
  [[maybe_unused]] const ssize_t r = ::write(STDERR_FILENO, buf, len);
}

}  // namespace hws::log
