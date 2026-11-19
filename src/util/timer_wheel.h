#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

namespace hws {

// Intrusive node embedded in objects that need timeouts.
struct TimerNode {
  TimerNode* prev = nullptr;
  TimerNode* next = nullptr;
  uint64_t expiry_tick = 0;
  bool timer_linked() const noexcept { return next != nullptr; }
};

// Hashed timing wheel: O(1) schedule/cancel, O(expired) advance. Timers further out than one
// revolution stay in their slot and are skipped until their tick comes around.
class TimerWheel {
 public:
  static constexpr size_t kSlots = 1024;  // power of two
  static constexpr size_t kMask = kSlots - 1;

  TimerWheel(uint64_t tick_ms, uint64_t now_ms) noexcept : tick_ms_(tick_ms), current_(now_ms / tick_ms) {
    for (auto& head : slots_) head.prev = head.next = &head;
  }
  TimerWheel(const TimerWheel&) = delete;
  TimerWheel& operator=(const TimerWheel&) = delete;

  uint64_t tick_ms() const noexcept { return tick_ms_; }
  size_t size() const noexcept { return count_; }

  // Schedules (or reschedules) n to fire at the first tick at or after deadline_ms.
  void schedule(TimerNode* n, uint64_t deadline_ms) noexcept {
    if (n->timer_linked()) unlink(n);
    uint64_t tick = (deadline_ms + tick_ms_ - 1) / tick_ms_;
    if (tick <= current_) tick = current_ + 1;
    n->expiry_tick = tick;
    link(n, slots_[tick & kMask]);
  }

  void cancel(TimerNode* n) noexcept {
    if (n->timer_linked()) unlink(n);
  }

  // Milliseconds until the next tick if any timer is pending, otherwise -1 (wait forever).
  int timeout_ms(uint64_t now_ms) const noexcept {
    if (count_ == 0) return -1;
    const uint64_t next = (current_ + 1) * tick_ms_;
    return now_ms >= next ? 0 : static_cast<int>(next - now_ms);
  }

  // Fires every timer due at or before now_ms. on_expire(TimerNode*) receives an unlinked node and
  // may reschedule it or any other node.
  template <class F>
  void advance(uint64_t now_ms, F&& on_expire) {
    const uint64_t target = now_ms / tick_ms_;
    if (target <= current_) return;
    const uint64_t first = current_ + 1;
    const uint64_t steps = std::min<uint64_t>(target - current_, kSlots);
    current_ = target;  // reschedules made from callbacks land strictly after target
    for (uint64_t t = first; t < first + steps; ++t) {
      TimerNode& head = slots_[t & kMask];
      if (head.next == &head) continue;
      // Detach the slot so callbacks can freely schedule into it.
      TimerNode pending;
      pending.next = head.next;
      pending.prev = head.prev;
      pending.next->prev = &pending;
      pending.prev->next = &pending;
      head.next = head.prev = &head;
      while (pending.next != &pending) {
        TimerNode* n = pending.next;
        n->prev->next = n->next;
        n->next->prev = n->prev;
        n->prev = n->next = nullptr;
        --count_;
        if (n->expiry_tick > target) {
          link(n, slots_[n->expiry_tick & kMask]);  // not due yet: a later revolution
        } else {
          on_expire(n);
        }
      }
    }
  }

 private:
  void link(TimerNode* n, TimerNode& head) noexcept {
    n->next = &head;
    n->prev = head.prev;
    head.prev->next = n;
    head.prev = n;
    ++count_;
  }
  void unlink(TimerNode* n) noexcept {
    n->prev->next = n->next;
    n->next->prev = n->prev;
    n->prev = n->next = nullptr;
    --count_;
  }

  uint64_t tick_ms_;
  uint64_t current_;
  size_t count_ = 0;
  std::array<TimerNode, kSlots> slots_{};
};

}  // namespace hws
