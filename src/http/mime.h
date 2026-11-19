#pragma once

#include <string_view>

namespace hws::http {

// Content-Type for a path, chosen by its extension (case-insensitive). Text types carry
// "; charset=utf-8". Unknown extensions get application/octet-stream.
std::string_view mime_type_for(std::string_view path) noexcept;

}  // namespace hws::http
