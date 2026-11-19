#pragma once

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace hws {

constexpr char ascii_lower(char c) noexcept {
  return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + ('a' - 'A')) : c;
}

inline bool iequals(std::string_view a, std::string_view b) noexcept {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    if (ascii_lower(a[i]) != ascii_lower(b[i])) return false;
  }
  return true;
}

inline bool istarts_with(std::string_view s, std::string_view prefix) noexcept {
  return s.size() >= prefix.size() && iequals(s.substr(0, prefix.size()), prefix);
}

inline std::string_view trim_ows(std::string_view s) noexcept {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
  return s;
}

// Parses 1*DIGIT. Rejects empty input, signs, whitespace and overflow.
inline bool parse_u64(std::string_view s, uint64_t& out) noexcept {
  if (s.empty()) return false;
  uint64_t v = 0;
  for (char c : s) {
    if (c < '0' || c > '9') return false;
    const auto d = static_cast<uint64_t>(c - '0');
    if (v > (UINT64_MAX - d) / 10) return false;
    v = v * 10 + d;
  }
  out = v;
  return true;
}

// Calls f(token) for each non-empty, OWS-trimmed element of a comma-separated list.
template <class F>
void for_each_list_item(std::string_view list, F&& f) {
  while (true) {
    const size_t comma = list.find(',');
    const std::string_view item = trim_ows(list.substr(0, comma));
    if (!item.empty()) f(item);
    if (comma == std::string_view::npos) return;
    list.remove_prefix(comma + 1);
  }
}

// Writes the decimal form of v into buf (needs 20 bytes). Returns the length.
inline size_t format_u64(char* buf, uint64_t v) noexcept {
  const auto r = std::to_chars(buf, buf + 20, v);
  return static_cast<size_t>(r.ptr - buf);
}

}  // namespace hws
