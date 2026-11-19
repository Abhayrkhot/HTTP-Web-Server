#pragma once

#include <sys/epoll.h>

#include <cstdint>
#include <functional>
#include <mutex>
#include <vector>

#include "util/fd.h"

namespace hws::net {

// Anything registered with the loop. epoll_event.data.ptr points at the handler, so dispatch is
// one virtual call with no fd->object lookup.
class IoHandler {
 public:
  virtual ~IoHandler() = default;
  virtual void on_io(uint32_t events) = 0;
};

// Thin epoll wrapper owned by one thread. post() is the only thread-safe entry point.
class EventLoop {
 public:
  EventLoop();  // throws std::system_error
  EventLoop(const EventLoop&) = delete;
  EventLoop& operator=(const EventLoop&) = delete;

  bool add(int fd, uint32_t events, IoHandler* handler) noexcept;
  bool modify(int fd, uint32_t events, IoHandler* handler) noexcept;
  void remove(int fd) noexcept;

  // Blocks for up to timeout_ms (-1 = forever) and returns the number of ready events.
  int wait(int timeout_ms) noexcept;
  // Invokes handlers for the events returned by the last wait().
  void dispatch(int count);

  // Queues fn to run on the loop thread and wakes the loop.
  void post(std::function<void()> fn);

 private:
  class Waker final : public IoHandler {
   public:
    explicit Waker(EventLoop& loop) noexcept : loop_(loop) {}
    void on_io(uint32_t events) override;

   private:
    EventLoop& loop_;
  };

  void run_posted();

  UniqueFd epfd_;
  UniqueFd wakefd_;
  std::vector<epoll_event> events_;
  std::mutex mu_;
  std::vector<std::function<void()>> posted_;
  Waker waker_{*this};
};

}  // namespace hws::net
