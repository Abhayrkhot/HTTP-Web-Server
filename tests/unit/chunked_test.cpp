#include "http/chunked.h"

#include <gtest/gtest.h>

#include <string>

using hws::Buffer;
using hws::http::ChunkedDecoder;
using Status = ChunkedDecoder::Status;

namespace {

struct Out {
  Status status;
  std::string body;
  size_t consumed;
  uint16_t error;
};

Out decode(const std::string& in, uint64_t max_body = 1 << 20) {
  ChunkedDecoder d;
  Buffer b;
  size_t used = 0;
  const Status st = d.feed(in.data(), in.size(), b, max_body, used);
  return {st, std::string(b.view()), used, d.error_status()};
}

}  // namespace

TEST(Chunked, Basic) {
  const Out o = decode("5\r\nhello\r\n7\r\n, world\r\n0\r\n\r\n");
  EXPECT_EQ(o.status, Status::Done);
  EXPECT_EQ(o.body, "hello, world");
}

TEST(Chunked, StopsAtEndOfBody) {
  const std::string body = "3\r\nabc\r\n0\r\n\r\n";
  const Out o = decode(body + "GET / HTTP/1.1\r\n");
  ASSERT_EQ(o.status, Status::Done);
  EXPECT_EQ(o.consumed, body.size());
}

TEST(Chunked, ByteAtATime) {
  const std::string in = "a;ext=1\r\n0123456789\r\n1\r\nX\r\n0\r\nTrailer: v\r\n\r\n";
  ChunkedDecoder d;
  Buffer b;
  for (size_t i = 0; i < in.size(); ++i) {
    size_t used = 0;
    const Status st = d.feed(in.data() + i, 1, b, 1 << 20, used);
    EXPECT_EQ(used, 1u);
    if (i + 1 < in.size()) ASSERT_EQ(st, Status::NeedMore) << i;
    else EXPECT_EQ(st, Status::Done);
  }
  EXPECT_EQ(b.view(), "0123456789X");
}

TEST(Chunked, ExtensionsAndTrailersIgnored) {
  const Out o = decode("4 ; name=\"v\"\r\nabcd\r\n0\r\nX-T: 1\r\nY: 2\r\n\r\n");
  EXPECT_EQ(o.status, Status::Done);
  EXPECT_EQ(o.body, "abcd");
}

TEST(Chunked, HexCaseInsensitive) {
  EXPECT_EQ(decode("A\r\n0123456789\r\n0\r\n\r\n").body, "0123456789");
  EXPECT_EQ(decode("a\r\n0123456789\r\n0\r\n\r\n").body, "0123456789");
}

TEST(Chunked, MalformedFraming) {
  EXPECT_EQ(decode("\r\nabc\r\n0\r\n\r\n").status, Status::Error);      // missing size
  EXPECT_EQ(decode("g\r\nabc\r\n0\r\n\r\n").status, Status::Error);     // non-hex
  EXPECT_EQ(decode("3\nabc\r\n0\r\n\r\n").status, Status::Error);       // bare LF after size
  EXPECT_EQ(decode("3\r\nabcX\r\n0\r\n\r\n").status, Status::Error);    // data longer than size
  EXPECT_EQ(decode("3\r\nabc\n0\r\n\r\n").status, Status::Error);       // bare LF after data
  EXPECT_EQ(decode("3\r\nabc\r\n0\r\n\n").status, Status::Error);       // bare LF at end
  EXPECT_EQ(decode("-3\r\nabc\r\n0\r\n\r\n").status, Status::Error);
  EXPECT_EQ(decode("3;a\nb\r\nabc\r\n0\r\n\r\n").status, Status::Error);  // LF inside extension
}

TEST(Chunked, SizeOverflowAndLimit) {
  const Out huge = decode("ffffffffffffffffff\r\n");
  EXPECT_EQ(huge.status, Status::Error);
  EXPECT_EQ(huge.error, 413);
  const Out over = decode("11\r\n", 16);  // 17 > 16
  EXPECT_EQ(over.status, Status::Error);
  EXPECT_EQ(over.error, 413);
  const Out cumulative = decode("8\r\n01234567\r\n9\r\n", 16);  // 8 + 9 > 16
  EXPECT_EQ(cumulative.status, Status::Error);
  EXPECT_EQ(cumulative.error, 413);
}

TEST(Chunked, ExtensionFloodRejected) {
  std::string in = "1;";
  in.append(ChunkedDecoder::kMaxMetaBytes + 10, 'x');
  EXPECT_EQ(decode(in).status, Status::Error);
}
