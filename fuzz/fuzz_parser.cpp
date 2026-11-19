// Parses a byte stream as a sequence of pipelined request heads and checks that every view the
// parser hands out lies inside the input, and that limits are honored.
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string_view>

#include "http/parser.h"

namespace {

bool inside(std::string_view outer, std::string_view v) {
  if (v.empty()) return true;
  return v.data() >= outer.data() && v.data() + v.size() <= outer.data() + outer.size();
}

void check(bool ok) {
  if (!ok) std::abort();
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  static auto* req = new hws::http::Request();
  hws::http::Limits limits;
  limits.max_header_bytes = 4096;
  limits.max_headers = 32;
  limits.max_uri_bytes = 1024;
  std::string_view in(reinterpret_cast<const char*>(data), size);
  for (int i = 0; i < 64 && !in.empty(); ++i) {
    const hws::http::ParseResult r = hws::http::parse_request_head(in, *req, limits);
    if (r.status != hws::http::ParseStatus::Complete) {
      if (r.status == hws::http::ParseStatus::Error) check(r.error_status >= 400 && r.error_status < 600);
      if (r.status == hws::http::ParseStatus::Incomplete) check(in.size() <= limits.max_header_bytes + 8);
      break;
    }
    check(r.consumed > 0 && r.consumed <= in.size());
    const std::string_view head = in.substr(0, r.consumed);
    check(head.size() <= limits.max_header_bytes + 8);
    check(req->header_count <= limits.max_headers);
    check(inside(head, req->method_token) && !req->method_token.empty());
    check(inside(head, req->target) && !req->target.empty());
    check(inside(head, req->host) && inside(head, req->query));
    check(req->path == "*" || req->path == "/" || inside(head, req->path));
    for (uint32_t h = 0; h < req->header_count; ++h) {
      check(inside(head, req->headers[h].name) && !req->headers[h].name.empty());
      check(inside(head, req->headers[h].value));
      for (char c : req->headers[h].value) check(c != '\r' && c != '\n' && c != '\0');
    }
    check(!(req->body_kind == hws::http::BodyKind::Length && req->content_length == 0));
    in.remove_prefix(r.consumed);
    // Skip a declared Content-Length body so the next head is parsed like a real connection would.
    if (req->body_kind == hws::http::BodyKind::Length) {
      if (req->content_length >= in.size()) break;
      in.remove_prefix(static_cast<size_t>(req->content_length));
    } else if (req->body_kind == hws::http::BodyKind::Chunked) {
      break;
    }
  }
  return 0;
}
