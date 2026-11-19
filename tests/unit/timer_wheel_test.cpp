#include "util/timer_wheel.h"

#include <gtest/gtest.h>

#include <vector>

using hws::TimerNode;
using hws::TimerWheel;

namespace {

struct Item : TimerNode {
  int id = 0;
  int fired = 0;
};

std::vector<int> advance(TimerWheel& w, uint64_t now) {
  std::vector<int> ids;
  w.advance(now, [&](TimerNode* n) {
    auto* it = static_cast<Item*>(n);
    ++it->fired;
    ids.push_back(it->id);
  });
  return ids;
}

}  // namespace

TEST(TimerWheel, FiresAtOrAfterDeadline) {
  TimerWheel w(100, 0);
  Item a;
  a.id = 1;
  w.schedule(&a, 250);
  EXPECT_EQ(w.size(), 1u);
  EXPECT_TRUE(advance(w, 200).empty());
  EXPECT_EQ(advance(w, 300), std::vector<int>{1});
  EXPECT_EQ(w.size(), 0u);
  EXPECT_FALSE(a.timer_linked());
}

TEST(TimerWheel, CancelAndReschedule) {
  TimerWheel w(100, 0);
  Item a;
  Item b;
  a.id = 1;
  b.id = 2;
  w.schedule(&a, 500);
  w.schedule(&b, 500);
  w.cancel(&a);
  w.schedule(&b, 900);  // move later
  EXPECT_TRUE(advance(w, 600).empty());
  EXPECT_EQ(advance(w, 1000), std::vector<int>{2});
  EXPECT_EQ(a.fired, 0);
}

TEST(TimerWheel, DeadlinesBeyondOneRevolution) {
  TimerWheel w(10, 0);
  const uint64_t far = 10 * TimerWheel::kSlots * 3 + 55;  // three revolutions out
  Item a;
  a.id = 7;
  w.schedule(&a, far);
  uint64_t t = 0;
  while (t + 10 < far) {
    t += 10;
    ASSERT_TRUE(advance(w, t).empty()) << t;
  }
  EXPECT_EQ(advance(w, far + 10), std::vector<int>{7});
}

TEST(TimerWheel, LongSleepFiresEverythingDue) {
  TimerWheel w(10, 0);
  std::vector<Item> items(50);
  for (size_t i = 0; i < items.size(); ++i) {
    items[i].id = static_cast<int>(i);
    w.schedule(&items[i], 100 + i * 1000);  // spread over ~5 revolutions
  }
  const auto fired = advance(w, 1'000'000);  // one huge jump
  EXPECT_EQ(fired.size(), items.size());
  EXPECT_EQ(w.size(), 0u);
}

TEST(TimerWheel, CallbackMayRescheduleIntoAnySlot) {
  TimerWheel w(100, 0);
  Item a;
  a.id = 1;
  w.schedule(&a, 100);
  int fires = 0;
  w.advance(150, [&](TimerNode* n) {
    ++fires;
    w.schedule(n, 0);  // "in the past": must land strictly after now, not re-fire immediately
  });
  EXPECT_EQ(fires, 1);
  EXPECT_TRUE(a.timer_linked());
  w.advance(250, [&](TimerNode*) { ++fires; });
  EXPECT_EQ(fires, 2);
}

TEST(TimerWheel, TimeoutHint) {
  TimerWheel w(100, 1000);
  EXPECT_EQ(w.timeout_ms(1000), -1);
  Item a;
  w.schedule(&a, 5000);
  EXPECT_EQ(w.timeout_ms(1000), 100);
  EXPECT_EQ(w.timeout_ms(1050), 50);
}
