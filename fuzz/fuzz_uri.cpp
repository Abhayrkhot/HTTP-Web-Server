// Path normalization must never produce a path that climbs out of the root or still contains
// dot segments, empty segments or control bytes.
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>

#include "http/uri.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  std::string raw = "/";
  raw.append(reinterpret_cast<const char*>(data), size);
  std::string out;
  if (!hws::http::normalize_path(raw, out)) return 0;
  if (out.empty() || out[0] != '/') std::abort();
  if (out.find("//") != std::string::npos) std::abort();
  // Every segment must be a real name.
  size_t start = 1;
  while (start < out.size()) {
    size_t end = out.find('/', start);
    if (end == std::string::npos) end = out.size();
    const std::string_view seg(out.data() + start, end - start);
    if (seg == "." || seg == "..") std::abort();
    start = end + 1;
  }
  for (char c : out) {
    const auto u = static_cast<unsigned char>(c);
    if (u < 0x20 || u == 0x7f) std::abort();
  }
  // A decoded result without '%' is a fixed point.
  if (out.find('%') == std::string::npos) {
    std::string again;
    if (!hws::http::normalize_path(out, again) || again != out) std::abort();
  }
  return 0;
}
