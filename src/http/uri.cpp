#include "http/uri.h"

#include "http/chars.h"

namespace hws::http {

bool normalize_path(std::string_view raw, std::string& out) {
  out.clear();
  if (raw.empty() || raw[0] != '/') return false;
  out.reserve(raw.size());
  out.push_back('/');

  // Walk segments of the raw path, decoding each one before interpreting "." and "..", so
  // "%2e%2e" is treated exactly like "..". A decoded '/' (%2F) also splits segments.
  std::string seg;
  size_t i = 1;
  bool trailing_slash = raw.size() > 1 && raw.back() == '/';
  auto flush_segment = [&](bool last) -> bool {
    if (seg.empty() || seg == ".") {
      if (last && seg == ".") trailing_slash = true;
      seg.clear();
      return true;
    }
    if (seg == "..") {
      if (out.size() <= 1) return false;  // would escape the root
      out.pop_back();                      // drop trailing '/'
      while (out.back() != '/') out.pop_back();
      if (last) trailing_slash = true;
      seg.clear();
      return true;
    }
    out.append(seg);
    out.push_back('/');
    seg.clear();
    return true;
  };

  while (i < raw.size()) {
    const char c = raw[i];
    if (c == '/') {
      if (!flush_segment(false)) return false;
      ++i;
      continue;
    }
    char decoded = c;
    if (c == '%') {
      if (i + 2 >= raw.size() || !is_hex(raw[i + 1]) || !is_hex(raw[i + 2])) return false;
      decoded = static_cast<char>(hex_value(raw[i + 1]) * 16 + hex_value(raw[i + 2]));
      i += 3;
      if (decoded == '/') {
        if (!flush_segment(false)) return false;
        continue;
      }
    } else {
      ++i;
    }
    const auto u = static_cast<unsigned char>(decoded);
    if (u < 0x20 || u == 0x7f) return false;  // NUL and control bytes never name a file
    seg.push_back(decoded);
  }
  if (!flush_segment(true)) return false;

  // `out` ends with '/' after every appended segment; drop it unless the request had one.
  if (out.size() > 1 && !trailing_slash) out.pop_back();
  return true;
}

bool has_dot_segment(std::string_view p) noexcept {
  for (size_t i = 0; i + 1 < p.size(); ++i) {
    if (p[i] == '/' && p[i + 1] == '.') return true;
  }
  return false;
}

}  // namespace hws::http
