#pragma once

#include <cstdint>
#include <string_view>

namespace hws::http {

enum class RangeResult : uint8_t {
  Ignore,          // no Range, unsupported unit, multiple ranges or bad syntax: send the full body
  Satisfiable,     // single range [start, start+length)
  Unsatisfiable,   // 416
};

// Parses a single byte range ("bytes=a-b", "bytes=a-", "bytes=-n") against a representation of
// `size` bytes. Multi-range requests are answered with the full body (RFC 9110 §14.2 allows it).
RangeResult parse_range(std::string_view header, uint64_t size, uint64_t& start, uint64_t& length) noexcept;

// If-None-Match evaluation with weak comparison (RFC 9110 §13.1.2). `etag` is our quoted tag.
bool if_none_match_matches(std::string_view header, std::string_view etag) noexcept;

// Strong comparison used by If-Range: weak tags never match.
bool strong_etag_equal(std::string_view a, std::string_view b) noexcept;

}  // namespace hws::http
