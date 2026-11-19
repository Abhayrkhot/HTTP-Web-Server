#pragma once

#include <cstdint>
#include <string_view>

namespace hws::http {

std::string_view reason_phrase(uint16_t status) noexcept;

// "HTTP/1.1 200 OK\r\n" for known codes. Unknown codes fall back to a thread-local buffer, so the
// view is valid until the next call on the same thread.
std::string_view status_line(uint16_t status) noexcept;

// 1xx, 204 and 304 responses never carry content (RFC 9110 §6.4.1).
constexpr bool status_has_body(uint16_t status) noexcept {
  return status >= 200 && status != 204 && status != 304;
}

}  // namespace hws::http
