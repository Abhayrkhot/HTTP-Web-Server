#include "http/range.h"

#include "util/strings.h"

namespace hws::http {
namespace {

std::string_view opaque_tag(std::string_view tag) noexcept {
  if (tag.size() >= 2 && tag[0] == 'W' && tag[1] == '/') tag.remove_prefix(2);
  return tag;
}

}  // namespace

RangeResult parse_range(std::string_view header, uint64_t size, uint64_t& start, uint64_t& length) noexcept {
  header = trim_ows(header);
  if (!istarts_with(header, "bytes=")) return RangeResult::Ignore;
  std::string_view spec = trim_ows(header.substr(6));
  if (spec.empty() || spec.find(',') != std::string_view::npos) return RangeResult::Ignore;
  const size_t dash = spec.find('-');
  if (dash == std::string_view::npos) return RangeResult::Ignore;
  const std::string_view first = trim_ows(spec.substr(0, dash));
  const std::string_view last = trim_ows(spec.substr(dash + 1));

  if (first.empty()) {  // suffix range: last N bytes
    uint64_t n = 0;
    if (!parse_u64(last, n)) return RangeResult::Ignore;
    if (n == 0 || size == 0) return RangeResult::Unsatisfiable;
    if (n > size) n = size;
    start = size - n;
    length = n;
    return RangeResult::Satisfiable;
  }
  uint64_t a = 0;
  if (!parse_u64(first, a)) return RangeResult::Ignore;
  uint64_t b = 0;
  if (last.empty()) {
    b = size == 0 ? 0 : size - 1;
  } else if (!parse_u64(last, b) || b < a) {
    return RangeResult::Ignore;
  }
  if (a >= size) return RangeResult::Unsatisfiable;
  if (b >= size) b = size - 1;
  start = a;
  length = b - a + 1;
  return RangeResult::Satisfiable;
}

bool if_none_match_matches(std::string_view header, std::string_view etag) noexcept {
  header = trim_ows(header);
  if (header == "*") return true;
  const std::string_view ours = opaque_tag(etag);
  bool matched = false;
  for_each_list_item(header, [&](std::string_view tag) { matched = matched || opaque_tag(tag) == ours; });
  return matched;
}

bool strong_etag_equal(std::string_view a, std::string_view b) noexcept {
  a = trim_ows(a);
  b = trim_ows(b);
  if (a.size() < 2 || b.size() < 2 || a[0] != '"' || b[0] != '"') return false;
  return a == b;
}

}  // namespace hws::http
