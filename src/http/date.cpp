#include "http/date.h"

#include <cstdio>
#include <cstring>

namespace hws::http {
namespace {

constexpr const char* kDays[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
constexpr const char* kLongDays[] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
constexpr const char* kMonths[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                   "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

void put2(char* p, int v) noexcept {
  p[0] = static_cast<char>('0' + v / 10);
  p[1] = static_cast<char>('0' + v % 10);
}

bool digits(std::string_view s, size_t pos, size_t n, int& out) noexcept {
  if (pos + n > s.size()) return false;
  int v = 0;
  for (size_t i = 0; i < n; ++i) {
    const char c = s[pos + i];
    if (c < '0' || c > '9') return false;
    v = v * 10 + (c - '0');
  }
  out = v;
  return true;
}

bool month(std::string_view s, size_t pos, int& out) noexcept {
  if (pos + 3 > s.size()) return false;
  for (int i = 0; i < 12; ++i) {
    if (s.compare(pos, 3, kMonths[i]) == 0) {
      out = i;
      return true;
    }
  }
  return false;
}

bool clock(std::string_view s, size_t pos, tm& t) noexcept {
  return digits(s, pos, 2, t.tm_hour) && pos + 8 <= s.size() && s[pos + 2] == ':' &&
         digits(s, pos + 3, 2, t.tm_min) && s[pos + 5] == ':' && digits(s, pos + 6, 2, t.tm_sec);
}

bool finish(tm& t, time_t& out) noexcept {
  if (t.tm_mday < 1 || t.tm_mday > 31 || t.tm_hour > 23 || t.tm_min > 59 || t.tm_sec > 60) return false;
  const time_t v = timegm(&t);
  if (v == static_cast<time_t>(-1)) return false;
  out = v;
  return true;
}

}  // namespace

void format_http_date(time_t t, char* buf) noexcept {
  tm g{};
  gmtime_r(&t, &g);
  std::memcpy(buf, kDays[g.tm_wday], 3);
  buf[3] = ',';
  buf[4] = ' ';
  put2(buf + 5, g.tm_mday);
  buf[7] = ' ';
  std::memcpy(buf + 8, kMonths[g.tm_mon], 3);
  buf[11] = ' ';
  const int year = g.tm_year + 1900;
  put2(buf + 12, year / 100);
  put2(buf + 14, year % 100);
  buf[16] = ' ';
  put2(buf + 17, g.tm_hour);
  buf[19] = ':';
  put2(buf + 20, g.tm_min);
  buf[22] = ':';
  put2(buf + 23, g.tm_sec);
  std::memcpy(buf + 25, " GMT", 4);
}

bool parse_http_date(std::string_view s, time_t& out) noexcept {
  tm t{};
  int year = 0;
  // IMF-fixdate: "Sun, 06 Nov 1994 08:49:37 GMT"
  if (s.size() == 29 && s[3] == ',') {
    if (s[4] != ' ' || !digits(s, 5, 2, t.tm_mday) || s[7] != ' ' || !month(s, 8, t.tm_mon) || s[11] != ' ' ||
        !digits(s, 12, 4, year) || s[16] != ' ' || !clock(s, 17, t) || s.substr(25) != " GMT") {
      return false;
    }
    t.tm_year = year - 1900;
    return finish(t, out);
  }
  // asctime: "Sun Nov  6 08:49:37 1994"
  if (s.size() == 24 && s[3] == ' ') {
    if (!month(s, 4, t.tm_mon) || s[7] != ' ') return false;
    const char d0 = s[8];
    if (d0 == ' ') {
      if (!digits(s, 9, 1, t.tm_mday)) return false;
    } else if (!digits(s, 8, 2, t.tm_mday)) {
      return false;
    }
    if (s[10] != ' ' || !clock(s, 11, t) || s[19] != ' ' || !digits(s, 20, 4, year)) return false;
    t.tm_year = year - 1900;
    return finish(t, out);
  }
  // RFC 850: "Sunday, 06-Nov-94 08:49:37 GMT"
  const size_t comma = s.find(',');
  if (comma != std::string_view::npos && comma >= 6 && comma <= 9) {
    bool known_day = false;
    for (const char* d : kLongDays) known_day = known_day || s.substr(0, comma) == d;
    if (!known_day) return false;
    const size_t p = comma + 2;
    if (s.size() != p + 22 || s[comma + 1] != ' ' || !digits(s, p, 2, t.tm_mday) || s[p + 2] != '-' ||
        !month(s, p + 3, t.tm_mon) || s[p + 6] != '-' || !digits(s, p + 7, 2, year) || s[p + 9] != ' ' ||
        !clock(s, p + 10, t) || s.substr(p + 18) != " GMT") {
      return false;
    }
    t.tm_year = (year < 70 ? year + 2000 : year + 1900) - 1900;
    return finish(t, out);
  }
  return false;
}

bool DateCache::update(time_t now) noexcept {
  if (now == cached_) return false;
  cached_ = now;
  format_http_date(now, http_);
  tm g{};
  gmtime_r(&now, &g);
  const int n = std::snprintf(log_, sizeof(log_), "%02d/%s/%04d:%02d:%02d:%02d +0000", g.tm_mday, kMonths[g.tm_mon],
                              g.tm_year + 1900, g.tm_hour, g.tm_min, g.tm_sec);
  log_len_ = n > 0 ? static_cast<size_t>(n) : 0;
  return true;
}

}  // namespace hws::http
