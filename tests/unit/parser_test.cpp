#include "http/parser.h"

#include <gtest/gtest.h>

#include <memory>
#include <string>

using hws::http::BodyKind;
using hws::http::Limits;
using hws::http::Method;
using hws::http::parse_request_head;
using hws::http::ParseResult;
using hws::http::ParseStatus;
using hws::http::Request;

namespace {

// Owns the input: Request views point into it, so it must outlive every assertion.
struct Parsed {
  std::unique_ptr<std::string> input;
  ParseResult result;
  std::unique_ptr<Request> req = std::make_unique<Request>();
};

Parsed parse(const std::string& s, Limits limits = {}) {
  Parsed p;
  p.input = std::make_unique<std::string>(s);
  p.result = parse_request_head(*p.input, *p.req, limits);
  return p;
}

uint16_t error_of(const std::string& s, Limits limits = {}) {
  const Parsed p = parse(s, limits);
  EXPECT_EQ(p.result.status, ParseStatus::Error) << s;
  return p.result.error_status;
}

}  // namespace

TEST(Parser, SimpleGet) {
  const std::string s = "GET /index.html?a=1&b=2 HTTP/1.1\r\nHost: example.com\r\nUser-Agent: t\r\n\r\n";
  const Parsed p = parse(s);
  ASSERT_EQ(p.result.status, ParseStatus::Complete);
  EXPECT_EQ(p.result.consumed, s.size());
  EXPECT_EQ(p.req->method, Method::Get);
  EXPECT_EQ(p.req->target, "/index.html?a=1&b=2");
  EXPECT_EQ(p.req->path, "/index.html");
  EXPECT_EQ(p.req->query, "a=1&b=2");
  EXPECT_EQ(p.req->host, "example.com");
  EXPECT_EQ(p.req->user_agent, "t");
  EXPECT_EQ(p.req->version_minor, 1);
  EXPECT_TRUE(p.req->keep_alive);
  EXPECT_EQ(p.req->body_kind, BodyKind::None);
  EXPECT_EQ(p.req->header_count, 2u);
}

TEST(Parser, IncompleteUntilTerminator) {
  const std::string full = "GET / HTTP/1.1\r\nHost: a\r\n\r\n";
  for (size_t n = 0; n < full.size(); ++n) {
    const Parsed p = parse(full.substr(0, n));
    EXPECT_EQ(p.result.status, ParseStatus::Incomplete) << n;
  }
  EXPECT_EQ(parse(full).result.status, ParseStatus::Complete);
}

TEST(Parser, ScanHintFindsTerminatorAcrossBoundary) {
  const std::string full = "GET / HTTP/1.1\r\nHost: a\r\n\r\n";
  Request req;
  for (size_t cut = 1; cut < full.size(); ++cut) {
    // Previously scanned `cut` bytes without finding the terminator.
    const ParseResult r = parse_request_head(full, req, {}, cut);
    EXPECT_EQ(r.status, ParseStatus::Complete) << cut;
  }
}

TEST(Parser, PipelinedRequestsConsumeOnlyTheFirstHead) {
  const std::string one = "GET /a HTTP/1.1\r\nHost: a\r\n\r\n";
  const Parsed p = parse(one + "GET /b HTTP/1.1\r\nHost: a\r\n\r\n");
  ASSERT_EQ(p.result.status, ParseStatus::Complete);
  EXPECT_EQ(p.result.consumed, one.size());
  EXPECT_EQ(p.req->path, "/a");
}

TEST(Parser, LeadingEmptyLinesAreIgnored) {
  const Parsed p = parse("\r\n\r\nGET / HTTP/1.1\r\nHost: a\r\n\r\n");
  EXPECT_EQ(p.result.status, ParseStatus::Complete);
}

TEST(Parser, Methods) {
  EXPECT_EQ(parse("HEAD / HTTP/1.1\r\nHost: a\r\n\r\n").req->method, Method::Head);
  EXPECT_EQ(parse("POST / HTTP/1.1\r\nHost: a\r\n\r\n").req->method, Method::Post);
  EXPECT_EQ(parse("DELETE / HTTP/1.1\r\nHost: a\r\n\r\n").req->method, Method::Delete);
  const Parsed custom = parse("PROPFIND / HTTP/1.1\r\nHost: a\r\n\r\n");
  EXPECT_EQ(custom.result.status, ParseStatus::Complete);
  EXPECT_EQ(custom.req->method, Method::Unknown);  // the server answers 501
  // Methods are case-sensitive.
  EXPECT_EQ(parse("get / HTTP/1.1\r\nHost: a\r\n\r\n").req->method, Method::Unknown);
}

TEST(Parser, Http10KeepAliveSemantics) {
  EXPECT_FALSE(parse("GET / HTTP/1.0\r\n\r\n").req->keep_alive);
  EXPECT_TRUE(parse("GET / HTTP/1.0\r\nConnection: keep-alive\r\n\r\n").req->keep_alive);
  EXPECT_FALSE(parse("GET / HTTP/1.1\r\nHost: a\r\nConnection: close\r\n\r\n").req->keep_alive);
  EXPECT_FALSE(parse("GET / HTTP/1.1\r\nHost: a\r\nConnection: upgrade, Close\r\n\r\n").req->keep_alive);
  EXPECT_FALSE(
      parse("GET / HTTP/1.0\r\nConnection: close\r\nConnection: keep-alive\r\n\r\n").req->keep_alive);
}

TEST(Parser, Http10DoesNotRequireHost) {
  EXPECT_EQ(parse("GET / HTTP/1.0\r\n\r\n").result.status, ParseStatus::Complete);
}

TEST(Parser, ContentLengthBody) {
  const Parsed p = parse("POST /x HTTP/1.1\r\nHost: a\r\nContent-Length: 42\r\n\r\n");
  ASSERT_EQ(p.result.status, ParseStatus::Complete);
  EXPECT_EQ(p.req->body_kind, BodyKind::Length);
  EXPECT_EQ(p.req->content_length, 42u);
  EXPECT_EQ(parse("POST /x HTTP/1.1\r\nHost: a\r\nContent-Length: 0\r\n\r\n").req->body_kind, BodyKind::None);
  // Repeated identical values are tolerated.
  EXPECT_EQ(parse("POST / HTTP/1.1\r\nHost: a\r\nContent-Length: 5\r\nContent-Length: 5\r\n\r\n").result.status,
            ParseStatus::Complete);
}

TEST(Parser, ChunkedBody) {
  const Parsed p = parse("POST / HTTP/1.1\r\nHost: a\r\nTransfer-Encoding: Chunked\r\n\r\n");
  ASSERT_EQ(p.result.status, ParseStatus::Complete);
  EXPECT_EQ(p.req->body_kind, BodyKind::Chunked);
}

TEST(Parser, ExpectContinue) {
  EXPECT_TRUE(parse("POST / HTTP/1.1\r\nHost: a\r\nExpect: 100-continue\r\nContent-Length: 3\r\n\r\n")
                  .req->expect_continue);
  EXPECT_EQ(error_of("POST / HTTP/1.1\r\nHost: a\r\nExpect: something\r\n\r\n"), 417);
}

TEST(Parser, TargetForms) {
  const Parsed abs = parse("GET http://Example.com:8080/p/q?x=1 HTTP/1.1\r\nHost: other\r\n\r\n");
  ASSERT_EQ(abs.result.status, ParseStatus::Complete);
  EXPECT_EQ(abs.req->host, "Example.com:8080");  // absolute-form authority wins
  EXPECT_EQ(abs.req->path, "/p/q");
  EXPECT_EQ(abs.req->query, "x=1");
  EXPECT_EQ(parse("GET http://h HTTP/1.1\r\nHost: h\r\n\r\n").req->path, "/");
  EXPECT_EQ(parse("OPTIONS * HTTP/1.1\r\nHost: a\r\n\r\n").req->path, "*");
  EXPECT_EQ(error_of("GET * HTTP/1.1\r\nHost: a\r\n\r\n"), 400);
  EXPECT_EQ(error_of("CONNECT example.com:443 HTTP/1.1\r\nHost: a\r\n\r\n"), 501);
  EXPECT_EQ(error_of("GET index.html HTTP/1.1\r\nHost: a\r\n\r\n"), 400);
  EXPECT_EQ(error_of("GET /a#frag HTTP/1.1\r\nHost: a\r\n\r\n"), 400);
}

// ---- framing errors: every one of these is a known request-smuggling or desync vector ----

TEST(ParserStrict, BothContentLengthAndTransferEncoding) {
  EXPECT_EQ(error_of("POST / HTTP/1.1\r\nHost: a\r\nContent-Length: 4\r\nTransfer-Encoding: chunked\r\n\r\n"), 400);
  EXPECT_EQ(error_of("POST / HTTP/1.1\r\nHost: a\r\nTransfer-Encoding: chunked\r\nContent-Length: 4\r\n\r\n"), 400);
}

TEST(ParserStrict, ConflictingContentLength) {
  EXPECT_EQ(error_of("POST / HTTP/1.1\r\nHost: a\r\nContent-Length: 4\r\nContent-Length: 5\r\n\r\n"), 400);
  EXPECT_EQ(error_of("POST / HTTP/1.1\r\nHost: a\r\nContent-Length: 4, 4\r\n\r\n"), 400);
  EXPECT_EQ(error_of("POST / HTTP/1.1\r\nHost: a\r\nContent-Length: +4\r\n\r\n"), 400);
  EXPECT_EQ(error_of("POST / HTTP/1.1\r\nHost: a\r\nContent-Length: 0x10\r\n\r\n"), 400);
  EXPECT_EQ(error_of("POST / HTTP/1.1\r\nHost: a\r\nContent-Length: 99999999999999999999999\r\n\r\n"), 400);
  EXPECT_EQ(error_of("POST / HTTP/1.1\r\nHost: a\r\nContent-Length: \r\n\r\n"), 400);
}

TEST(ParserStrict, TransferEncodingVariants) {
  EXPECT_EQ(error_of("POST / HTTP/1.1\r\nHost: a\r\nTransfer-Encoding: gzip, chunked\r\n\r\n"), 501);
  EXPECT_EQ(error_of("POST / HTTP/1.1\r\nHost: a\r\nTransfer-Encoding: chunked, gzip\r\n\r\n"), 400);
  EXPECT_EQ(error_of("POST / HTTP/1.1\r\nHost: a\r\nTransfer-Encoding: xchunked\r\n\r\n"), 400);
  EXPECT_EQ(error_of("POST / HTTP/1.1\r\nHost: a\r\nTransfer-Encoding: chunked\r\nTransfer-Encoding: chunked\r\n\r\n"),
            400);
  EXPECT_EQ(error_of("POST / HTTP/1.0\r\nTransfer-Encoding: chunked\r\n\r\n"), 400);
}

TEST(ParserStrict, WhitespaceBeforeColonAndObsFold) {
  EXPECT_EQ(error_of("GET / HTTP/1.1\r\nHost: a\r\nContent-Length : 5\r\n\r\n"), 400);
  EXPECT_EQ(error_of("GET / HTTP/1.1\r\nHost: a\r\nX-A: b\r\n  folded\r\n\r\n"), 400);
  EXPECT_EQ(error_of("GET / HTTP/1.1\r\n Host: a\r\n\r\n"), 400);
}

TEST(ParserStrict, BareLfAndBareCr) {
  EXPECT_EQ(error_of("GET / HTTP/1.1\nHost: a\r\n\r\n"), 400);
  EXPECT_EQ(error_of("GET / HTTP/1.1\r\nHost: a\nX: y\r\n\r\n"), 400);
  EXPECT_EQ(error_of("GET / HTTP/1.1\r\nX: a\rb\r\nHost: a\r\n\r\n"), 400);
}

TEST(ParserStrict, InvalidBytes) {
  std::string nul = "GET / HTTP/1.1\r\nHost: a\r\nX: a";
  nul.push_back('\0');
  nul.append("b\r\n\r\n");
  EXPECT_EQ(error_of(nul), 400);
  EXPECT_EQ(error_of("GET / HTTP/1.1\r\nHost: a\r\nX\x01: a\r\n\r\n"), 400);
  EXPECT_EQ(error_of("GET /\x7f HTTP/1.1\r\nHost: a\r\n\r\n"), 400);
  EXPECT_EQ(error_of("G\"T / HTTP/1.1\r\nHost: a\r\n\r\n"), 400);
}

TEST(ParserStrict, RequestLine) {
  EXPECT_EQ(error_of("GET  / HTTP/1.1\r\nHost: a\r\n\r\n"), 400);
  EXPECT_EQ(error_of("GET / HTTP/1.1 \r\nHost: a\r\n\r\n"), 400);
  EXPECT_EQ(error_of("GET /\r\nHost: a\r\n\r\n"), 400);
  EXPECT_EQ(error_of("GET / HTTP/2.0\r\nHost: a\r\n\r\n"), 505);
  EXPECT_EQ(error_of("GET / HTTP/1.x\r\nHost: a\r\n\r\n"), 400);
  EXPECT_EQ(error_of("GET / http/1.1\r\nHost: a\r\n\r\n"), 400);
}

TEST(ParserStrict, Host) {
  EXPECT_EQ(error_of("GET / HTTP/1.1\r\n\r\n"), 400);
  EXPECT_EQ(error_of("GET / HTTP/1.1\r\nHost: a\r\nHost: b\r\n\r\n"), 400);
  EXPECT_EQ(error_of("GET / HTTP/1.1\r\nHost: a/b\r\n\r\n"), 400);
  EXPECT_EQ(error_of("GET / HTTP/1.1\r\nHost: a b\r\n\r\n"), 400);
  EXPECT_EQ(parse("GET / HTTP/1.1\r\nHost: [::1]:80\r\n\r\n").result.status, ParseStatus::Complete);
}

TEST(ParserLimits, HeaderBytesAndCount) {
  Limits lim;
  lim.max_header_bytes = 256;
  EXPECT_EQ(error_of(std::string("GET / HTTP/1.1\r\nHost: a\r\nX: ").append(300, 'a').append("\r\n\r\n"), lim), 431);
  // No terminator yet but already over the limit: fail early instead of buffering forever.
  EXPECT_EQ(error_of(std::string("GET / HTTP/1.1\r\nHost: a\r\nX: ").append(300, 'a'), lim), 431);
  EXPECT_EQ(error_of(std::string("GET /").append(300, 'a'), lim), 414);

  Limits few;
  few.max_headers = 3;
  EXPECT_EQ(error_of("GET / HTTP/1.1\r\nHost: a\r\nA: 1\r\nB: 2\r\nC: 3\r\n\r\n", few), 431);

  Limits uri;
  uri.max_uri_bytes = 10;
  EXPECT_EQ(error_of("GET /0123456789abc HTTP/1.1\r\nHost: a\r\n\r\n", uri), 414);
}

TEST(Parser, HeaderLookupIsCaseInsensitiveAndTrimmed) {
  const Parsed p = parse("GET / HTTP/1.1\r\nHost: a\r\nX-Thing: \t value with spaces \t\r\nEmpty:\r\n\r\n");
  ASSERT_EQ(p.result.status, ParseStatus::Complete);
  EXPECT_EQ(p.req->header("x-thing"), "value with spaces");
  EXPECT_EQ(p.req->header("EMPTY"), "");
  EXPECT_EQ(p.req->header("missing"), "");
}

TEST(Parser, ConditionalHeadersCaptured) {
  const Parsed p = parse(
      "GET / HTTP/1.1\r\nHost: a\r\nIf-None-Match: \"x\"\r\nIf-Modified-Since: Sun, 06 Nov 1994 08:49:37 GMT\r\n"
      "Range: bytes=0-1\r\nIf-Range: \"x\"\r\nReferer: r\r\n\r\n");
  ASSERT_EQ(p.result.status, ParseStatus::Complete);
  EXPECT_EQ(p.req->if_none_match, "\"x\"");
  EXPECT_EQ(p.req->if_modified_since, "Sun, 06 Nov 1994 08:49:37 GMT");
  EXPECT_EQ(p.req->range, "bytes=0-1");
  EXPECT_EQ(p.req->if_range, "\"x\"");
  EXPECT_EQ(p.req->referer, "r");
}

TEST(Parser, ObsTextInValuesIsAccepted) {
  EXPECT_EQ(parse("GET / HTTP/1.1\r\nHost: a\r\nX: caf\xc3\xa9\r\n\r\n").result.status, ParseStatus::Complete);
}
