#include "http/parser.h"

#include <cstring>

#include "http/chars.h"
#include "util/strings.h"

namespace hws::http {
namespace {

Method lookup_method(std::string_view m) noexcept {
  switch (m.size()) {
    case 3:
      if (m == "GET") return Method::Get;
      if (m == "PUT") return Method::Put;
      break;
    case 4:
      if (m == "HEAD") return Method::Head;
      if (m == "POST") return Method::Post;
      break;
    case 5:
      if (m == "PATCH") return Method::Patch;
      if (m == "TRACE") return Method::Trace;
      break;
    case 6:
      if (m == "DELETE") return Method::Delete;
      break;
    case 7:
      if (m == "OPTIONS") return Method::Options;
      if (m == "CONNECT") return Method::Connect;
      break;
    default:
      break;
  }
  return Method::Unknown;
}

ParseResult fail(uint16_t status, const char* reason) noexcept {
  ParseResult r;
  r.status = ParseStatus::Error;
  r.error_status = status;
  r.error_reason = reason;
  return r;
}

bool valid_host(std::string_view h) noexcept {
  for (char c : h) {
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' ||
                    c == '.' || c == ':' || c == '[' || c == ']' || c == '_' || c == '%';
    if (!ok) return false;
  }
  return true;
}

void reset_request(Request& req) noexcept {
  req.method = Method::Unknown;
  req.method_token = {};
  req.target = req.path = req.query = req.host = {};
  req.version_minor = 1;
  req.keep_alive = true;
  req.expect_continue = false;
  req.body_kind = BodyKind::None;
  req.content_length = 0;
  req.connection = req.if_none_match = req.if_modified_since = req.if_range = req.range = {};
  req.user_agent = req.referer = {};
  req.body = {};
  req.header_count = 0;
}

}  // namespace

std::string_view method_name(Method m) noexcept {
  switch (m) {
    case Method::Get: return "GET";
    case Method::Head: return "HEAD";
    case Method::Post: return "POST";
    case Method::Put: return "PUT";
    case Method::Delete: return "DELETE";
    case Method::Options: return "OPTIONS";
    case Method::Patch: return "PATCH";
    case Method::Connect: return "CONNECT";
    case Method::Trace: return "TRACE";
    case Method::Unknown: break;
  }
  return "UNKNOWN";
}

std::string_view Request::header(std::string_view name) const noexcept {
  for (uint32_t i = 0; i < header_count; ++i) {
    if (iequals(headers[i].name, name)) return headers[i].value;
  }
  return {};
}

ParseResult parse_request_head(std::string_view data, Request& req, const Limits& limits,
                               size_t scan_from) noexcept {
  // RFC 9112 §2.2: ignore a few empty lines before the request-line.
  // Anything else that is not a method token (bare LF, lone CR) fails the request-line parse.
  size_t start = 0;
  while (start + 1 < data.size() && data[start] == '\r' && data[start + 1] == '\n') {
    start += 2;
    if (start > 8) return fail(400, "malformed request line");
  }
  const std::string_view d = data.substr(start);
  if (d.empty()) return {};

  // Locate the end of the head. Re-scan the last 3 bytes of the previous attempt in case the
  // terminator straddles the boundary.
  size_t from = scan_from > start + 3 ? scan_from - start - 3 : 0;
  if (from > d.size()) from = d.size();
  const void* term = memmem(d.data() + from, d.size() - from, "\r\n\r\n", 4);
  if (term == nullptr) {
    if (d.size() > limits.max_header_bytes) {
      const bool have_line = std::memchr(d.data(), '\n', d.size()) != nullptr;
      return have_line ? fail(431, "request header fields too large") : fail(414, "uri too long");
    }
    return {};
  }
  const size_t head_len = static_cast<size_t>(static_cast<const char*>(term) - d.data()) + 4;
  if (head_len > limits.max_header_bytes) return fail(431, "request header fields too large");

  reset_request(req);
  const char* p = d.data();
  const char* const end = d.data() + head_len - 2;  // the CRLF of the terminating empty line

  // request-line = method SP request-target SP HTTP-version CRLF
  const char* tok = p;
  while (is_tchar(*p)) ++p;
  if (p == tok || *p != ' ') return fail(400, "malformed request line");
  req.method_token = {tok, static_cast<size_t>(p - tok)};
  req.method = lookup_method(req.method_token);
  ++p;

  tok = p;
  while (is_target_char(*p)) ++p;
  if (p == tok || *p != ' ') return fail(400, "malformed request target");
  if (static_cast<size_t>(p - tok) > limits.max_uri_bytes) return fail(414, "uri too long");
  req.target = {tok, static_cast<size_t>(p - tok)};
  ++p;

  if (end - p < 10 || std::memcmp(p, "HTTP/", 5) != 0 || p[5] < '0' || p[5] > '9' || p[6] != '.' ||
      p[7] < '0' || p[7] > '9' || p[8] != '\r' || p[9] != '\n') {
    return fail(400, "malformed http version");
  }
  if (p[5] != '1') return fail(505, "http version not supported");
  req.version_minor = p[7] == '0' ? 0 : 1;
  req.keep_alive = req.version_minor == 1;  // HTTP/1.0 closes unless it asks for keep-alive
  p += 10;

  bool seen_host = false;
  bool seen_cl = false;
  bool seen_te = false;
  bool conn_close = false;
  std::string_view transfer_encoding;

  while (p < end) {
    if (*p == ' ' || *p == '\t') return fail(400, "obsolete line folding");
    const char* name_begin = p;
    while (is_tchar(*p)) ++p;
    if (p == name_begin || *p != ':') return fail(400, "malformed header name");
    const std::string_view name{name_begin, static_cast<size_t>(p - name_begin)};
    ++p;
    while (*p == ' ' || *p == '\t') ++p;
    const char* value_begin = p;
    while (is_field_value_char(*p)) ++p;
    if (p[0] != '\r' || p[1] != '\n') return fail(400, "malformed header value");
    const char* value_end = p;
    while (value_end > value_begin && (value_end[-1] == ' ' || value_end[-1] == '\t')) --value_end;
    const std::string_view value{value_begin, static_cast<size_t>(value_end - value_begin)};
    p += 2;

    if (req.header_count >= limits.max_headers || req.header_count >= kMaxHeadersHard) {
      return fail(431, "too many header fields");
    }
    req.headers[req.header_count++] = Header{name, value};

    switch (name.size()) {
      case 4:
        if (iequals(name, "host")) {
          if (seen_host) return fail(400, "duplicate host header");
          if (!valid_host(value)) return fail(400, "invalid host header");
          seen_host = true;
          req.host = value;
        }
        break;
      case 5:
        if (iequals(name, "range")) req.range = value;
        break;
      case 6:
        if (iequals(name, "expect")) {
          if (iequals(value, "100-continue")) req.expect_continue = true;
          else if (!value.empty()) return fail(417, "unsupported expectation");
        }
        break;
      case 7:
        if (iequals(name, "referer")) req.referer = value;
        break;
      case 8:
        if (iequals(name, "if-range")) req.if_range = value;
        break;
      case 10:
        if (iequals(name, "connection")) {
          // Several Connection fields are legal; "close" anywhere wins.
          for_each_list_item(value, [&](std::string_view t) {
            if (iequals(t, "close")) conn_close = true;
            else if (iequals(t, "keep-alive") && req.version_minor == 0) req.keep_alive = true;
          });
          if (conn_close) req.keep_alive = false;
          req.connection = value;
        } else if (iequals(name, "user-agent")) {
          req.user_agent = value;
        }
        break;
      case 13:
        if (iequals(name, "if-none-match")) req.if_none_match = value;
        break;
      case 14:
        if (iequals(name, "content-length")) {
          uint64_t n = 0;
          if (!parse_u64(value, n) || n > (uint64_t{1} << 62)) return fail(400, "invalid content-length");
          if (seen_cl && n != req.content_length) return fail(400, "conflicting content-length");
          seen_cl = true;
          req.content_length = n;
        }
        break;
      case 17:
        if (iequals(name, "transfer-encoding")) {
          if (seen_te) return fail(400, "duplicate transfer-encoding");
          seen_te = true;
          transfer_encoding = value;
        } else if (iequals(name, "if-modified-since")) {
          req.if_modified_since = value;
        }
        break;
      default:
        break;
    }
  }

  if (req.version_minor == 1 && !seen_host) return fail(400, "missing host header");

  if (seen_te) {
    // RFC 9112 §6.1/§6.3: TE + CL is a smuggling vector; chunked must be the final coding.
    if (seen_cl) return fail(400, "both transfer-encoding and content-length");
    if (req.version_minor == 0) return fail(400, "transfer-encoding in http/1.0");
    const std::string_view te = trim_ows(transfer_encoding);
    if (iequals(te, "chunked")) {
      req.body_kind = BodyKind::Chunked;
    } else {
      const size_t comma = te.rfind(',');
      const std::string_view last = trim_ows(comma == std::string_view::npos ? te : te.substr(comma + 1));
      if (iequals(last, "chunked")) return fail(501, "unsupported transfer-coding");
      return fail(400, "chunked must be the final transfer-coding");
    }
  } else if (seen_cl && req.content_length > 0) {
    req.body_kind = BodyKind::Length;
  }

  // request-target forms (RFC 9112 §3.2)
  const std::string_view t = req.target;
  if (t[0] == '/') {
    const size_t q = t.find('?');
    req.path = t.substr(0, q);
    if (q != std::string_view::npos) req.query = t.substr(q + 1);
  } else if (t == "*") {
    if (req.method != Method::Options) return fail(400, "asterisk-form requires OPTIONS");
    req.path = t;
  } else if (istarts_with(t, "http://") || istarts_with(t, "https://")) {
    const size_t scheme_end = t.find("://") + 3;
    const size_t auth_end = t.find_first_of("/?", scheme_end);
    const std::string_view authority = t.substr(scheme_end, auth_end - scheme_end);
    if (authority.empty() || !valid_host(authority)) return fail(400, "invalid absolute-form authority");
    req.host = authority;  // RFC 9112 §3.2.2: the target's authority wins over Host
    if (auth_end == std::string_view::npos) {
      req.path = "/";
    } else if (t[auth_end] == '?') {
      req.path = "/";
      req.query = t.substr(auth_end + 1);
    } else {
      const std::string_view rest = t.substr(auth_end);
      const size_t q = rest.find('?');
      req.path = rest.substr(0, q);
      if (q != std::string_view::npos) req.query = rest.substr(q + 1);
    }
  } else if (req.method == Method::Connect) {
    return fail(501, "connect not supported");
  } else {
    return fail(400, "malformed request target");
  }

  ParseResult r;
  r.status = ParseStatus::Complete;
  r.consumed = start + head_len;
  return r;
}

}  // namespace hws::http
