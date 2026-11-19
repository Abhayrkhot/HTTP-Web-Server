#include "http/uri.h"

#include <gtest/gtest.h>

#include <string>

using hws::http::has_dot_segment;
using hws::http::normalize_path;

namespace {

std::string norm(const std::string& in) {
  std::string out;
  EXPECT_TRUE(normalize_path(in, out)) << in;
  return out;
}

bool rejects(const std::string& in) {
  std::string out;
  return !normalize_path(in, out);
}

}  // namespace

TEST(Uri, Identity) {
  EXPECT_EQ(norm("/"), "/");
  EXPECT_EQ(norm("/a/b.html"), "/a/b.html");
  EXPECT_EQ(norm("/a/b/"), "/a/b/");
}

TEST(Uri, DotSegmentsAndDuplicateSlashes) {
  EXPECT_EQ(norm("/a/./b"), "/a/b");
  EXPECT_EQ(norm("/a/b/../c"), "/a/c");
  EXPECT_EQ(norm("//a///b"), "/a/b");
  EXPECT_EQ(norm("/a/b/.."), "/a/");
  EXPECT_EQ(norm("/a/."), "/a/");
  EXPECT_EQ(norm("/a/b/../../"), "/");
}

TEST(Uri, PercentDecoding) {
  EXPECT_EQ(norm("/hello%20world"), "/hello world");
  EXPECT_EQ(norm("/%41%62c"), "/Abc");
  EXPECT_EQ(norm("/a%2fb"), "/a/b");            // an encoded slash is a separator
  EXPECT_EQ(norm("/caf%C3%A9"), "/caf\xc3\xa9");
}

TEST(Uri, TraversalIsRejectedNotClamped) {
  EXPECT_TRUE(rejects("/.."));
  EXPECT_TRUE(rejects("/../etc/passwd"));
  EXPECT_TRUE(rejects("/a/../../etc/passwd"));
  EXPECT_TRUE(rejects("/%2e%2e/etc/passwd"));
  EXPECT_TRUE(rejects("/%2E%2E%2Fetc%2Fpasswd"));
  EXPECT_TRUE(rejects("/a/%2e%2e/%2e%2e/b"));
  EXPECT_TRUE(rejects("/..%2f..%2fetc"));
}

TEST(Uri, MalformedAndDangerous) {
  EXPECT_TRUE(rejects(""));
  EXPECT_TRUE(rejects("a/b"));
  EXPECT_TRUE(rejects("/%"));
  EXPECT_TRUE(rejects("/%4"));
  EXPECT_TRUE(rejects("/%zz"));
  EXPECT_TRUE(rejects("/a%00b"));   // NUL would truncate the path in syscalls
  EXPECT_TRUE(rejects("/a%0ab"));   // control bytes
  EXPECT_TRUE(rejects("/a%7f"));
}

TEST(Uri, DotFiles) {
  EXPECT_TRUE(has_dot_segment("/.git/config"));
  EXPECT_TRUE(has_dot_segment("/a/.env"));
  EXPECT_FALSE(has_dot_segment("/a/b.c"));
  EXPECT_FALSE(has_dot_segment("/"));
}
