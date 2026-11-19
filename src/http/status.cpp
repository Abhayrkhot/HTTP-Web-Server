#include "http/status.h"

#include <cstdio>

namespace hws::http {

#define HWS_STATUSES(X)                     \
  X(100, "Continue")                        \
  X(101, "Switching Protocols")             \
  X(200, "OK")                              \
  X(201, "Created")                         \
  X(202, "Accepted")                        \
  X(204, "No Content")                      \
  X(206, "Partial Content")                 \
  X(301, "Moved Permanently")               \
  X(302, "Found")                           \
  X(303, "See Other")                       \
  X(304, "Not Modified")                    \
  X(307, "Temporary Redirect")              \
  X(308, "Permanent Redirect")              \
  X(400, "Bad Request")                     \
  X(401, "Unauthorized")                    \
  X(403, "Forbidden")                       \
  X(404, "Not Found")                       \
  X(405, "Method Not Allowed")              \
  X(408, "Request Timeout")                 \
  X(409, "Conflict")                        \
  X(411, "Length Required")                 \
  X(413, "Content Too Large")               \
  X(414, "URI Too Long")                    \
  X(415, "Unsupported Media Type")          \
  X(416, "Range Not Satisfiable")           \
  X(417, "Expectation Failed")              \
  X(421, "Misdirected Request")             \
  X(429, "Too Many Requests")               \
  X(431, "Request Header Fields Too Large") \
  X(500, "Internal Server Error")           \
  X(501, "Not Implemented")                 \
  X(502, "Bad Gateway")                     \
  X(503, "Service Unavailable")             \
  X(504, "Gateway Timeout")                 \
  X(505, "HTTP Version Not Supported")

std::string_view reason_phrase(uint16_t status) noexcept {
  switch (status) {
#define X(code, text) \
  case code:          \
    return text;
    HWS_STATUSES(X)
#undef X
    default:
      return "Unknown";
  }
}

std::string_view status_line(uint16_t status) noexcept {
  switch (status) {
#define X(code, text) \
  case code:          \
    return "HTTP/1.1 " #code " " text "\r\n";
    HWS_STATUSES(X)
#undef X
    default:
      break;
  }
  thread_local char buf[32];
  const int n = std::snprintf(buf, sizeof(buf), "HTTP/1.1 %03u Unknown\r\n", static_cast<unsigned>(status % 1000));
  return {buf, n > 0 ? static_cast<size_t>(n) : 0};
}

#undef HWS_STATUSES

}  // namespace hws::http
