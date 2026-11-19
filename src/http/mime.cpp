#include "http/mime.h"

#include "util/strings.h"

namespace hws::http {
namespace {

struct MimeEntry {
  std::string_view ext;
  std::string_view type;
};

constexpr MimeEntry kTypes[] = {
    {"html", "text/html; charset=utf-8"},
    {"htm", "text/html; charset=utf-8"},
    {"css", "text/css; charset=utf-8"},
    {"js", "text/javascript; charset=utf-8"},
    {"mjs", "text/javascript; charset=utf-8"},
    {"json", "application/json"},
    {"map", "application/json"},
    {"txt", "text/plain; charset=utf-8"},
    {"md", "text/markdown; charset=utf-8"},
    {"csv", "text/csv; charset=utf-8"},
    {"xml", "application/xml"},
    {"svg", "image/svg+xml"},
    {"png", "image/png"},
    {"jpg", "image/jpeg"},
    {"jpeg", "image/jpeg"},
    {"gif", "image/gif"},
    {"webp", "image/webp"},
    {"avif", "image/avif"},
    {"ico", "image/x-icon"},
    {"bmp", "image/bmp"},
    {"woff", "font/woff"},
    {"woff2", "font/woff2"},
    {"ttf", "font/ttf"},
    {"otf", "font/otf"},
    {"wasm", "application/wasm"},
    {"pdf", "application/pdf"},
    {"zip", "application/zip"},
    {"gz", "application/gzip"},
    {"tar", "application/x-tar"},
    {"mp3", "audio/mpeg"},
    {"ogg", "audio/ogg"},
    {"wav", "audio/wav"},
    {"mp4", "video/mp4"},
    {"webm", "video/webm"},
    {"bin", "application/octet-stream"},
};

}  // namespace

std::string_view mime_type_for(std::string_view path) noexcept {
  const size_t slash = path.rfind('/');
  const size_t dot = path.rfind('.');
  if (dot == std::string_view::npos || (slash != std::string_view::npos && dot < slash)) {
    return "application/octet-stream";
  }
  const std::string_view ext = path.substr(dot + 1);
  for (const auto& e : kTypes) {
    if (iequals(e.ext, ext)) return e.type;
  }
  return "application/octet-stream";
}

}  // namespace hws::http
