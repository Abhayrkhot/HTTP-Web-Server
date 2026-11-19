#pragma once

#include <netinet/in.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "http/date.h"
#include "http/parser.h"
#include "http/response.h"
#include "net/event_loop.h"
#include "server/access_log.h"
#include "server/config.h"
#include "server/file_cache.h"
#include "server/metrics.h"
#include "server/static_files.h"
#include "util/buffer.h"
#include "util/fd.h"
#include "util/timer_wheel.h"

namespace hws {

class Connection;
class Server;

// One event-loop thread. Shared-nothing: a worker owns its listening sockets (SO_REUSEPORT) or
// shares the server's with EPOLLEXCLUSIVE, and owns every connection it accepts for its whole
// life, so the request path takes no locks.
class Worker {
 public:
  Worker(Server& server, unsigned id, std::vector<int> listen_fds, bool owns_listeners);
  ~Worker();
  Worker(const Worker&) = delete;
  Worker& operator=(const Worker&) = delete;

  void start(std::optional<unsigned> cpu);
  void join();

  // Thread-safe controls (posted to the loop).
  void drain(uint64_t timeout_ms);
  void stop();
  void reopen_logs();

  const WorkerMetrics& metrics() const noexcept { return metrics_; }
  unsigned id() const noexcept { return id_; }

  // ---- used by Connection, on the worker thread only ----
  net::EventLoop& loop() noexcept { return loop_; }
  TimerWheel& timers() noexcept { return timers_; }
  uint64_t now_ms() const noexcept { return now_ms_; }
  char* in_scratch() noexcept { return in_scratch_.get(); }
  size_t in_scratch_size() const noexcept { return kInScratch; }
  Buffer& out_scratch() noexcept { return out_scratch_; }
  http::Request& request() noexcept { return *request_; }
  http::Response& response() noexcept { return response_; }
  const http::Limits& limits() const noexcept { return limits_; }
  const Config& cfg() const noexcept { return cfg_; }
  WorkerMetrics& metrics() noexcept { return metrics_; }
  std::string_view common_headers() const noexcept { return common_headers_; }
  const http::DateCache& date() const noexcept { return date_; }
  AccessLog& access_log() noexcept { return access_log_; }
  bool draining() const noexcept { return draining_; }
  void schedule_ready(Connection* c);
  void on_closed(Connection* c);
  void handle(const http::Request& req, http::Response& resp);

 private:
  class Listener final : public net::IoHandler {
   public:
    Listener(Worker& w, int fd) noexcept : w_(w), fd_(fd) {}
    void on_io(uint32_t) override { w_.accept_ready(fd_); }
    int fd() const noexcept { return fd_; }

   private:
    Worker& w_;
    int fd_;
  };

  static constexpr size_t kInScratch = 64 * 1024;

  void run(std::optional<unsigned> cpu);
  void accept_ready(int listen_fd);
  void shed_one(int listen_fd);
  void reject_busy(int fd) noexcept;
  void run_ready();
  void reap();
  void begin_drain(uint64_t timeout_ms);
  void close_listeners();
  void abort_all();
  void update_date();

  Server& server_;
  const Config& cfg_;
  unsigned id_;
  std::vector<int> listen_fds_;
  bool owns_listeners_;
  std::vector<std::unique_ptr<Listener>> listeners_;
  net::EventLoop loop_;
  TimerWheel timers_;
  uint64_t now_ms_;
  uint64_t max_connections_;
  uint64_t live_count_ = 0;
  Connection* live_head_ = nullptr;
  std::vector<Connection*> ready_;
  std::vector<Connection*> ready_running_;
  std::vector<Connection*> graveyard_;
  std::unique_ptr<char[]> in_scratch_;
  Buffer out_scratch_;
  std::unique_ptr<http::Request> request_;
  http::Response response_;
  http::Limits limits_;
  http::DateCache date_;
  std::string common_headers_;
  std::string path_;  // normalized request path scratch
  std::unique_ptr<FileCache> file_cache_;
  std::unique_ptr<StaticFiles> static_;
  AccessLog access_log_;
  UniqueFd reserve_fd_;  // spare descriptor released to shed connections on EMFILE
  uint64_t last_housekeeping_ms_ = 0;
  uint64_t last_emfile_log_ms_ = 0;
  bool draining_ = false;
  bool stop_ = false;
  uint64_t drain_deadline_ms_ = 0;
  WorkerMetrics metrics_;
  std::thread thread_;
};

}  // namespace hws
