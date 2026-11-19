// Range parsing must stay in bounds; HTTP dates that parse must round-trip; If-None-Match and
// config parsers must not crash on arbitrary input.
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>

#include "http/date.h"
#include "http/range.h"
#include "server/config.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  if (size < 4) return 0;
  uint32_t file_size = 0;
  std::memcpy(&file_size, data, 4);
  file_size %= 1u << 24;
  const std::string_view s(reinterpret_cast<const char*>(data + 4), size - 4);

  uint64_t start = 0;
  uint64_t len = 0;
  if (hws::http::parse_range(s, file_size, start, len) == hws::http::RangeResult::Satisfiable) {
    if (len == 0 || start >= file_size || start + len > file_size) std::abort();
  }

  time_t t = 0;
  if (hws::http::parse_http_date(s, t) && t >= 0 && t < 253402300800) {  // through year 9999
    char buf[hws::http::kHttpDateLen];
    hws::http::format_http_date(t, buf);
    time_t back = 0;
    if (!hws::http::parse_http_date(std::string_view(buf, sizeof(buf)), back) || back != t) std::abort();
  }

  (void)hws::http::if_none_match_matches(s, "\"5f3-1a\"");
  uint64_t v = 0;
  (void)hws::parse_duration_ms(s, v);
  (void)hws::parse_size(s, v);
  return 0;
}
