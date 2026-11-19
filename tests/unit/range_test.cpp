#include "http/range.h"

#include <gtest/gtest.h>

using hws::http::if_none_match_matches;
using hws::http::parse_range;
using hws::http::RangeResult;
using hws::http::strong_etag_equal;

TEST(Range, Forms) {
  uint64_t s = 0;
  uint64_t n = 0;
  ASSERT_EQ(parse_range("bytes=0-9", 100, s, n), RangeResult::Satisfiable);
  EXPECT_EQ(s, 0u);
  EXPECT_EQ(n, 10u);
  ASSERT_EQ(parse_range("bytes=90-", 100, s, n), RangeResult::Satisfiable);
  EXPECT_EQ(s, 90u);
  EXPECT_EQ(n, 10u);
  ASSERT_EQ(parse_range("bytes=-5", 100, s, n), RangeResult::Satisfiable);
  EXPECT_EQ(s, 95u);
  EXPECT_EQ(n, 5u);
  ASSERT_EQ(parse_range("bytes=50-1000", 100, s, n), RangeResult::Satisfiable);  // clamped
  EXPECT_EQ(n, 50u);
  ASSERT_EQ(parse_range("bytes=-500", 100, s, n), RangeResult::Satisfiable);  // whole body
  EXPECT_EQ(s, 0u);
  EXPECT_EQ(n, 100u);
  ASSERT_EQ(parse_range("Bytes = 1-1", 100, s, n), RangeResult::Ignore);  // OWS not allowed around '='
}

TEST(Range, Unsatisfiable) {
  uint64_t s = 0;
  uint64_t n = 0;
  EXPECT_EQ(parse_range("bytes=100-", 100, s, n), RangeResult::Unsatisfiable);
  EXPECT_EQ(parse_range("bytes=200-300", 100, s, n), RangeResult::Unsatisfiable);
  EXPECT_EQ(parse_range("bytes=-0", 100, s, n), RangeResult::Unsatisfiable);
  EXPECT_EQ(parse_range("bytes=0-", 0, s, n), RangeResult::Unsatisfiable);
}

TEST(Range, IgnoredForms) {
  uint64_t s = 0;
  uint64_t n = 0;
  EXPECT_EQ(parse_range("items=0-1", 100, s, n), RangeResult::Ignore);
  EXPECT_EQ(parse_range("bytes=0-1,5-6", 100, s, n), RangeResult::Ignore);  // multi-range: full body
  EXPECT_EQ(parse_range("bytes=5-1", 100, s, n), RangeResult::Ignore);
  EXPECT_EQ(parse_range("bytes=a-b", 100, s, n), RangeResult::Ignore);
  EXPECT_EQ(parse_range("bytes=", 100, s, n), RangeResult::Ignore);
  EXPECT_EQ(parse_range("bytes=-", 100, s, n), RangeResult::Ignore);
}

TEST(ETag, IfNoneMatchUsesWeakComparison) {
  EXPECT_TRUE(if_none_match_matches("\"abc\"", "\"abc\""));
  EXPECT_TRUE(if_none_match_matches("W/\"abc\"", "\"abc\""));
  EXPECT_TRUE(if_none_match_matches("\"x\", W/\"abc\" , \"y\"", "\"abc\""));
  EXPECT_TRUE(if_none_match_matches("*", "\"abc\""));
  EXPECT_FALSE(if_none_match_matches("\"abd\"", "\"abc\""));
  EXPECT_FALSE(if_none_match_matches("", "\"abc\""));
}

TEST(ETag, StrongComparison) {
  EXPECT_TRUE(strong_etag_equal("\"a\"", "\"a\""));
  EXPECT_FALSE(strong_etag_equal("W/\"a\"", "\"a\""));
  EXPECT_FALSE(strong_etag_equal("\"a\"", "\"b\""));
}
