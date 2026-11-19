#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace hws::http {

enum class Method : uint8_t { Get, Head, Post, Put, Delete, Options, Patch, Connect, Trace, Unknown };
std::string_view method_name(Method m) noexcept;

struct Header {
  std::string_view name;
  std::string_view value;
};

struct Limits {
  size_t max_header_bytes = 16 * 1024;  // request line + header fields + CRLFs
  size_t max_headers = 100;
  size_t max_uri_bytes = 8 * 1024;
};

enum class BodyKind : uint8_t { None, Length, Chunked };

inline constexpr size_t kMaxHeadersHard = 256;

// A parsed request head. Every view points into the caller's input buffer, so a Request is
// only valid while those bytes are alive and unmodified.
struct Request {
  Method method = Method::Unknown;
  std::string_view method_token;
  std::string_view target;  // raw request-target
  std::string_view path;    // raw path (not percent-decoded); "*" for asterisk-form
  std::string_view query;   // after '?', without it
  std::string_view host;    // Host header, or the authority of an absolute-form target
  uint8_t version_minor = 1;
  bool keep_alive = true;
  bool expect_continue = false;
  BodyKind body_kind = BodyKind::None;
  uint64_t content_length = 0;

  // Fields the server consults on the hot path, captured during parsing.
  std::string_view connection;
  std::string_view if_none_match;
  std::string_view if_modified_since;
  std::string_view if_range;
  std::string_view range;
  std::string_view user_agent;
  std::string_view referer;

  std::string_view body;  // filled in by the connection once the body is complete

  uint32_t header_count = 0;
  std::array<Header, kMaxHeadersHard> headers;

  bool is_http10() const noexcept { return version_minor == 0; }
  // Case-insensitive lookup of the first header with this name; empty if absent.
  std::string_view header(std::string_view name) const noexcept;
};

enum class ParseStatus : uint8_t { Complete, Incomplete, Error };

struct ParseResult {
  ParseStatus status = ParseStatus::Incomplete;
  size_t consumed = 0;          // bytes of the head, including skipped leading CRLFs and final CRLFCRLF
  uint16_t error_status = 0;    // status code to answer with when status == Error
  const char* error_reason = "";
};

// Parses an HTTP/1.x request head from the start of `data`. The parser is strict about framing
// (RFC 9112): bare LF, obs-fold, whitespace before ':', invalid bytes, conflicting
// Content-Length/Transfer-Encoding and duplicate Host are all errors, which closes the door on
// request smuggling. `scan_from` lets the caller skip bytes already searched for CRLFCRLF.
ParseResult parse_request_head(std::string_view data, Request& req, const Limits& limits,
                               size_t scan_from = 0) noexcept;

}  // namespace hws::http
