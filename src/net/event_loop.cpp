#include "net/event_loop.h"

#include <sys/eventfd.h>
#include <unistd.h>

#include <cerrno>
#include <system_error>

namespace hws::net {

EventLoop::EventLoop()
    : epfd_(::epoll_create1(EPOLL_CLOEXEC)), wakefd_(::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC)), events_(512) {
  if (!epfd_) throw std::system_error(errno, std::generic_category(), "epoll_create1");
  if (!wakefd_) throw std::system_error(errno, std::generic_category(), "eventfd");
  if (!add(wakefd_.get(), EPOLLIN, &waker_)) throw std::system_error(errno, std::generic_category(), "epoll_ctl");
}

bool EventLoop::add(int fd, uint32_t events, IoHandler* handler) noexcept {
  epoll_event ev{};
  ev.events = events;
  ev.data.ptr = handler;
  return ::epoll_ctl(epfd_.get(), EPOLL_CTL_ADD, fd, &ev) == 0;
}

bool EventLoop::modify(int fd, uint32_t events, IoHandler* handler) noexcept {
  epoll_event ev{};
  ev.events = events;
  ev.data.ptr = handler;
  return ::epoll_ctl(epfd_.get(), EPOLL_CTL_MOD, fd, &ev) == 0;
}

void EventLoop::remove(int fd) noexcept { ::epoll_ctl(epfd_.get(), EPOLL_CTL_DEL, fd, nullptr); }

int EventLoop::wait(int timeout_ms) noexcept {
  const int n = ::epoll_wait(epfd_.get(), events_.data(), static_cast<int>(events_.size()), timeout_ms);
  return n < 0 ? 0 : n;  // EINTR: treat as a spurious wakeup
}

void EventLoop::dispatch(int count) {
  for (int i = 0; i < count; ++i) {
    auto* h = static_cast<IoHandler*>(events_[static_cast<size_t>(i)].data.ptr);
    h->on_io(events_[static_cast<size_t>(i)].events);
  }
}

void EventLoop::post(std::function<void()> fn) {
  {
    std::lock_guard<std::mutex> lock(mu_);
    posted_.push_back(std::move(fn));
  }
  const uint64_t one = 1;
  [[maybe_unused]] const ssize_t r = ::write(wakefd_.get(), &one, sizeof(one));
}

void EventLoop::run_posted() {
  std::vector<std::function<void()>> tasks;
  {
    std::lock_guard<std::mutex> lock(mu_);
    tasks.swap(posted_);
  }
  for (auto& t : tasks) t();
}

void EventLoop::Waker::on_io(uint32_t) {
  uint64_t v = 0;
  while (::read(loop_.wakefd_.get(), &v, sizeof(v)) > 0) {
  }
  loop_.run_posted();
}

}  // namespace hws::net
