#pragma once

#include <string>
#include <string_view>

namespace hws::http {

// Percent-decodes a request path and removes dot-segments and empty segments (RFC 3986 §5.2.4).
// The result always starts with '/' and keeps a trailing '/' when the input had one.
// Fails (returns false) on malformed escapes, encoded NUL or control bytes, or a ".." that would
// climb above the root — callers answer 400 rather than silently clamping.
bool normalize_path(std::string_view raw, std::string& out);

// True when any segment of a normalized path starts with '.', e.g. "/.git/config".
bool has_dot_segment(std::string_view normalized) noexcept;

}  // namespace hws::http
