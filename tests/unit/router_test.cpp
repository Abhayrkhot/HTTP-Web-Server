#include "http/router.h"

#include <gtest/gtest.h>

using hws::http::kGet;
using hws::http::kPost;
using hws::http::Method;
using hws::http::Request;
using hws::http::Response;
using hws::http::Router;

TEST(Router, ExactBeatsPrefixAndLongestPrefixWins) {
  Router r;
  int hit = 0;
  r.add("/api/health", kGet, [&](const Request&, Response&) { hit = 1; });
  r.add_prefix("/api/", kGet, [&](const Request&, Response&) { hit = 2; });
  r.add_prefix("/api/v2/", kGet, [&](const Request&, Response&) { hit = 3; });
  Request req;
  Response resp;
  r.find("/api/health", Method::Get).route->handler(req, resp);
  EXPECT_EQ(hit, 1);
  r.find("/api/other", Method::Get).route->handler(req, resp);
  EXPECT_EQ(hit, 2);
  r.find("/api/v2/x", Method::Get).route->handler(req, resp);
  EXPECT_EQ(hit, 3);
  EXPECT_EQ(r.find("/nope", Method::Get).route, nullptr);
}

TEST(Router, MethodMatching) {
  Router r;
  r.add("/x", kGet, [](const Request&, Response&) {});
  r.add("/y", kPost, [](const Request&, Response&) {});
  EXPECT_FALSE(r.find("/x", Method::Get).method_not_allowed);
  EXPECT_FALSE(r.find("/x", Method::Head).method_not_allowed);  // GET implies HEAD
  EXPECT_TRUE(r.find("/x", Method::Post).method_not_allowed);
  EXPECT_TRUE(r.find("/y", Method::Get).method_not_allowed);
  EXPECT_EQ(Router::allow_header(kGet | kPost), "GET, HEAD, POST");
}

TEST(Response, RejectsHeaderInjection) {
  Response resp;
  EXPECT_TRUE(resp.add_header("X-Ok", "value"));
  EXPECT_FALSE(resp.add_header("X-Bad", "a\r\nSet-Cookie: evil=1"));
  EXPECT_FALSE(resp.add_header("X-Bad", std::string_view("a\0b", 3)));
  EXPECT_FALSE(resp.add_header("Bad Name", "v"));
  EXPECT_FALSE(resp.add_header("", "v"));
  EXPECT_EQ(resp.headers(), "X-Ok: value\r\n");
}
