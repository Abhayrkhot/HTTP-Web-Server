#pragma once

#include <ctime>
#include <string_view>

namespace hws::http {

inline constexpr size_t kHttpDateLen = 29;  // "Sun, 06 Nov 1994 08:49:37 GMT"

// Writes an IMF-fixdate (RFC 9110 §5.6.7) into buf, which must hold kHttpDateLen bytes.
void format_http_date(time_t t, char* buf) noexcept;

// Accepts IMF-fixdate, RFC 850 and asctime formats, as RFC 9110 requires of recipients.
bool parse_http_date(std::string_view s, time_t& out) noexcept;

// Per-thread cache of the current second's Date header and access-log timestamp.
class DateCache {
 public:
  // Returns true when the second changed and the cached strings were rebuilt.
  bool update(time_t now) noexcept;
  std::string_view http_date() const noexcept { return {http_, kHttpDateLen}; }
  std::string_view log_time() const noexcept { return {log_, log_len_}; }  // "19/Nov/2026:16:00:00 +0000"
  time_t now() const noexcept { return cached_; }

 private:
  time_t cached_ = -1;
  char http_[kHttpDateLen + 1] = {};
  char log_[40] = {};
  size_t log_len_ = 0;
};

}  // namespace hws::http
