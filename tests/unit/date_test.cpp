#include "http/date.h"

#include <gtest/gtest.h>

#include <string>

using hws::http::DateCache;
using hws::http::format_http_date;
using hws::http::kHttpDateLen;
using hws::http::parse_http_date;

TEST(HttpDate, FormatsImfFixdate) {
  char buf[kHttpDateLen];
  format_http_date(784111777, buf);
  EXPECT_EQ(std::string(buf, kHttpDateLen), "Sun, 06 Nov 1994 08:49:37 GMT");
  format_http_date(0, buf);
  EXPECT_EQ(std::string(buf, kHttpDateLen), "Thu, 01 Jan 1970 00:00:00 GMT");
}

TEST(HttpDate, ParsesAllThreeFormats) {
  time_t t = 0;
  ASSERT_TRUE(parse_http_date("Sun, 06 Nov 1994 08:49:37 GMT", t));
  EXPECT_EQ(t, 784111777);
  ASSERT_TRUE(parse_http_date("Sunday, 06-Nov-94 08:49:37 GMT", t));
  EXPECT_EQ(t, 784111777);
  ASSERT_TRUE(parse_http_date("Sun Nov  6 08:49:37 1994", t));
  EXPECT_EQ(t, 784111777);
}

TEST(HttpDate, RejectsGarbage) {
  time_t t = 0;
  EXPECT_FALSE(parse_http_date("", t));
  EXPECT_FALSE(parse_http_date("yesterday", t));
  EXPECT_FALSE(parse_http_date("Sun, 06 Nov 1994 08:49:37 UTC", t));
  EXPECT_FALSE(parse_http_date("Sun, 06 Foo 1994 08:49:37 GMT", t));
  EXPECT_FALSE(parse_http_date("Sun, 36 Nov 1994 08:49:37 GMT", t));
  EXPECT_FALSE(parse_http_date("Sun, 06 Nov 1994 28:49:37 GMT", t));
  EXPECT_FALSE(parse_http_date("Funday, 06-Nov-94 08:49:37 GMT", t));
}

TEST(HttpDate, RoundTrip) {
  char buf[kHttpDateLen];
  for (time_t t : {time_t{1}, time_t{951782400}, time_t{1700000000}, time_t{4102444799}}) {
    format_http_date(t, buf);
    time_t back = 0;
    ASSERT_TRUE(parse_http_date(std::string_view(buf, kHttpDateLen), back));
    EXPECT_EQ(back, t);
  }
}

TEST(HttpDate, CacheRebuildsOncePerSecond) {
  DateCache c;
  EXPECT_TRUE(c.update(784111777));
  EXPECT_FALSE(c.update(784111777));
  EXPECT_EQ(c.http_date(), "Sun, 06 Nov 1994 08:49:37 GMT");
  EXPECT_EQ(c.log_time(), "06/Nov/1994:08:49:37 +0000");
  EXPECT_TRUE(c.update(784111778));
}
