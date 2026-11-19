#include "server/worker.h"

#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <exception>
#include <stdexcept>

#include "http/router.h"
#include "http/uri.h"
#include "net/socket.h"
#include "server/connection.h"
#include "server/server.h"
#include "util/clock.h"
#include "util/log.h"

namespace hws {
namespace {

constexpr int kAcceptBatch = 64;
constexpr uint64_t kTickMs = 100;

}  // namespace

Worker::Worker(Server& server, unsigned id, std::vector<int> listen_fds, bool owns_listeners)
    : server_(server),
      cfg_(server.config()),
      id_(id),
      listen_fds_(std::move(listen_fds)),
      owns_listeners_(owns_listeners),
      timers_(kTickMs, monotonic_ms()),
      now_ms_(monotonic_ms()),
      max_connections_(server.per_worker_max_connections()),
      in_scratch_(new char[kInScratch]),
      request_(std::make_unique<http::Request>()),
      reserve_fd_(::open("/dev/null", O_RDONLY | O_CLOEXEC)) {
  limits_.max_header_bytes = cfg_.max_header_bytes;
  limits_.max_headers = cfg_.max_headers;
  limits_.max_uri_bytes = cfg_.max_uri_bytes;
  if (server.root_fd() >= 0) {
    FileCacheOptions fo;
    fo.max_entries = cfg_.file_cache_entries;
    fo.ttl_ms = cfg_.file_cache_ttl_ms;
    fo.inline_max = cfg_.inline_file_max;
    fo.inline_budget = cfg_.inline_cache_bytes;
    fo.follow_symlinks = cfg_.follow_symlinks;
    file_cache_ = std::make_unique<FileCache>(server.root_fd(), fo);
    static_ = std::make_unique<StaticFiles>(StaticOptions{cfg_.index, cfg_.serve_dotfiles, cfg_.cache_control});
  }
  std::string err;
  if (!access_log_.open(cfg_.access_log, err)) LOG_ERROR("worker %u: %s", id_, err.c_str());
  out_scratch_.ensure_writable(64 * 1024);
  ready_.reserve(64);
  ready_running_.reserve(64);
  update_date();
}

Worker::~Worker() {
  join();
  if (owns_listeners_) {
    for (int fd : listen_fds_) ::close(fd);
  }
}

void Worker::start(std::optional<unsigned> cpu) {
  thread_ = std::thread([this, cpu] {
    try {
      run(cpu);
    } catch (const std::exception& e) {
      LOG_ERROR("worker %u crashed: %s", id_, e.what());
      server_.on_worker_failed();
    }
    server_.on_worker_exit();
  });
}

void Worker::join() {
  if (thread_.joinable()) thread_.join();
}

void Worker::drain(uint64_t timeout_ms) {
  loop_.post([this, timeout_ms] { begin_drain(timeout_ms); });
}

void Worker::stop() {
  loop_.post([this] { stop_ = true; });
}

void Worker::reopen_logs() {
  loop_.post([this] { access_log_.reopen(); });
}

void Worker::update_date() {
  if (!date_.update(realtime_sec())) return;
  common_headers_.assign("Server: ");
  common_headers_.append(cfg_.server_name);
  common_headers_.append("\r\nDate: ");
  common_headers_.append(date_.http_date());
  common_headers_.append("\r\n");
}

void Worker::run(std::optional<unsigned> cpu) {
  char tag[16];
  std::snprintf(tag, sizeof(tag), "w%u", id_);
  log::set_thread_tag(tag);
  if (cpu) {
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(*cpu, &set);
    if (pthread_setaffinity_np(pthread_self(), sizeof(set), &set) != 0) LOG_WARN("cannot pin to cpu %u", *cpu);
  }
  const uint32_t listen_events = EPOLLIN | (owns_listeners_ ? 0u : static_cast<uint32_t>(EPOLLEXCLUSIVE));
  for (int fd : listen_fds_) {
    listeners_.push_back(std::make_unique<Listener>(*this, fd));
    if (!loop_.add(fd, listen_events, listeners_.back().get())) {
      throw std::runtime_error(std::string("epoll_ctl(listener): ") + std::strerror(errno));
    }
  }

  while (!stop_) {
    int timeout = timers_.timeout_ms(now_ms_);
    if (!ready_.empty()) timeout = 0;
    if (access_log_.enabled() || draining_) timeout = timeout < 0 ? 1000 : std::min(timeout, 1000);
    const int n = loop_.wait(timeout);
    now_ms_ = monotonic_ms();
    update_date();
    loop_.dispatch(n);
    run_ready();
    timers_.advance(now_ms_, [](TimerNode* node) { static_cast<Connection*>(node)->on_timer(); });
    if (now_ms_ - last_housekeeping_ms_ >= 1000) {
      last_housekeeping_ms_ = now_ms_;
      access_log_.maybe_flush(now_ms_);
    }
    reap();
    if (draining_) {
      if (live_count_ == 0) break;
      if (now_ms_ >= drain_deadline_ms_) {
        LOG_WARN("drain deadline reached; closing %llu connections", static_cast<unsigned long long>(live_count_));
        break;
      }
    }
  }
  close_listeners();
  abort_all();
  reap();
  access_log_.flush();
}

void Worker::accept_ready(int listen_fd) {
  for (int i = 0; i < kAcceptBatch && !draining_; ++i) {
    sockaddr_in6 peer{};
    socklen_t len = sizeof(peer);
    const int fd = ::accept4(listen_fd, reinterpret_cast<sockaddr*>(&peer), &len, SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (fd < 0) {
      switch (errno) {
        case EINTR:
        case ECONNABORTED:
        case EPROTO:
          continue;
        case EAGAIN:
          return;
        case EMFILE:
        case ENFILE:
          shed_one(listen_fd);
          continue;
        default:
          LOG_ERROR("accept4: %s", std::strerror(errno));
          return;
      }
    }
    if (live_count_ >= max_connections_) {
      reject_busy(fd);
      continue;
    }
    net::set_nodelay(fd);
    auto* c = new Connection(*this, fd, peer);
    c->next_live = live_head_;
    if (live_head_ != nullptr) live_head_->prev_live = c;
    live_head_ = c;
    ++live_count_;
    metrics_.accepted.add();
    if (!c->start()) c->abort();
  }
}

void Worker::shed_one(int listen_fd) {
  // Out of descriptors: free the spare, accept and immediately close one pending connection so
  // the listen queue keeps moving instead of spinning, then take the spare back.
  reserve_fd_.reset();
  const int fd = ::accept4(listen_fd, nullptr, nullptr, SOCK_CLOEXEC);
  if (fd >= 0) ::close(fd);
  reserve_fd_.reset(::open("/dev/null", O_RDONLY | O_CLOEXEC));
  metrics_.rejected_fd.add();
  if (now_ms_ - last_emfile_log_ms_ >= 1000) {
    last_emfile_log_ms_ = now_ms_;
    LOG_WARN("out of file descriptors (RLIMIT_NOFILE); shedding connections");
  }
}

void Worker::reject_busy(int fd) noexcept {
  static constexpr char k503[] =
      "HTTP/1.1 503 Service Unavailable\r\nContent-Length: 0\r\nConnection: close\r\nRetry-After: 1\r\n\r\n";
  [[maybe_unused]] const ssize_t n = ::send(fd, k503, sizeof(k503) - 1, MSG_NOSIGNAL | MSG_DONTWAIT);
  ::close(fd);
  metrics_.rejected_limit.add();
}

void Worker::schedule_ready(Connection* c) {
  if (c->queued_ready) return;
  c->queued_ready = true;
  ready_.push_back(c);
}

void Worker::run_ready() {
  if (ready_.empty()) return;
  ready_running_.swap(ready_);
  for (Connection* c : ready_running_) {
    c->queued_ready = false;
    if (!c->closed()) c->on_ready();
  }
  ready_running_.clear();
}

void Worker::on_closed(Connection* c) {
  if (c->prev_live != nullptr) c->prev_live->next_live = c->next_live;
  else live_head_ = c->next_live;
  if (c->next_live != nullptr) c->next_live->prev_live = c->prev_live;
  c->prev_live = c->next_live = nullptr;
  --live_count_;
  metrics_.closed.add();
  if (c->queued_ready) {
    ready_.erase(std::remove(ready_.begin(), ready_.end(), c), ready_.end());
    c->queued_ready = false;
  }
  graveyard_.push_back(c);  // deleted after this loop turn: stale events in the batch may name it
}

void Worker::reap() {
  for (Connection* c : graveyard_) delete c;
  graveyard_.clear();
}

void Worker::close_listeners() {
  for (auto& l : listeners_) {
    loop_.remove(l->fd());
    if (owns_listeners_) ::close(l->fd());
  }
  if (owns_listeners_) listen_fds_.clear();
  listeners_.clear();
}

void Worker::begin_drain(uint64_t timeout_ms) {
  if (draining_) return;
  // Take whatever is already queued on our listeners so those clients get answered (with
  // Connection: close) instead of being reset when the sockets close.
  for (auto& l : listeners_) accept_ready(l->fd());
  draining_ = true;
  drain_deadline_ms_ = monotonic_ms() + timeout_ms;
  close_listeners();
  for (Connection* c = live_head_; c != nullptr;) {
    Connection* next = c->next_live;
    c->begin_drain();
    c = next;
  }
}

void Worker::abort_all() {
  while (live_head_ != nullptr) live_head_->abort();
}

void Worker::handle(const http::Request& req, http::Response& resp) {
  using http::Method;
  if (req.method == Method::Unknown || req.method == Method::Connect || req.method == Method::Trace) {
    resp.text(501, "Not Implemented\n");
    return;
  }
  if (req.path == "*") {  // OPTIONS *
    resp.status = 204;
    resp.add_header("Allow", "GET, HEAD, POST, PUT, DELETE, PATCH, OPTIONS");
    return;
  }
  if (!http::normalize_path(req.path, path_)) {
    resp.text(400, "Bad Request: invalid path\n");
    return;
  }
  const http::Router::Match m = server_.router().find(path_, req.method);
  if (m.route != nullptr) {
    if (m.method_not_allowed) {
      resp.text(405, "Method Not Allowed\n");
      resp.add_header("Allow", http::Router::allow_header(m.route->methods));
      return;
    }
    try {
      m.route->handler(req, resp);
    } catch (const std::exception& e) {
      metrics_.handler_errors.add();
      LOG_ERROR("handler for %s threw: %s", m.route->path.c_str(), e.what());
      resp.reset();
      resp.text(500, "Internal Server Error\n");
    }
    return;
  }
  if (static_) {
    static_->serve(req, path_, *file_cache_, now_ms_, resp, metrics_);
    return;
  }
  resp.text(404, "Not Found\n");
}

}  // namespace hws
