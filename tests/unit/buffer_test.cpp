#include "util/buffer.h"

#include <gtest/gtest.h>

#include <string>

using hws::Buffer;

TEST(Buffer, StartsEmptyWithoutAllocating) {
  Buffer b;
  EXPECT_TRUE(b.empty());
  EXPECT_EQ(b.capacity(), 0u);
}

TEST(Buffer, AppendConsumeAndReuse) {
  Buffer b;
  b.append("hello ");
  b.append("world");
  b.append_u64(42);
  EXPECT_EQ(b.view(), "hello world42");
  b.consume(6);
  EXPECT_EQ(b.view(), "world42");
  b.consume(7);
  EXPECT_TRUE(b.empty());
  const size_t cap = b.capacity();
  b.append("x");
  EXPECT_EQ(b.capacity(), cap);  // cursor reset on empty, no regrowth
}

TEST(Buffer, CompactsBeforeGrowing) {
  Buffer b;
  b.ensure_writable(1024);
  const size_t cap = b.capacity();
  b.append(std::string(cap - 10, 'a'));
  b.consume(cap - 20);  // 10 bytes left, large consumed prefix
  b.ensure_writable(cap - 100);
  EXPECT_EQ(b.capacity(), cap);
  EXPECT_EQ(b.view(), std::string(10, 'a'));
}

TEST(Buffer, GrowsPreservingContent) {
  Buffer b;
  std::string expect;
  for (int i = 0; i < 1000; ++i) {
    const std::string chunk = std::to_string(i) + ",";
    b.append(chunk);
    expect += chunk;
  }
  EXPECT_EQ(b.view(), expect);
}

TEST(Buffer, MoveAndRelease) {
  Buffer a;
  a.append("abc");
  Buffer b(std::move(a));
  EXPECT_EQ(b.view(), "abc");
  EXPECT_EQ(a.capacity(), 0u);  // NOLINT(bugprone-use-after-move): moved-from state is defined
  b.release();
  EXPECT_EQ(b.capacity(), 0u);
  EXPECT_TRUE(b.empty());
}
