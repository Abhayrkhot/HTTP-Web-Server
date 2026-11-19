#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "http/router.h"
#include "server/config.h"
#include "util/fd.h"

namespace hws {

class Worker;

// Owns configuration, listening sockets and workers, and turns signals into lifecycle actions.
// Register routes on router() after init() and before run().
class Server {
 public:
  explicit Server(Config cfg);
  ~Server();
  Server(const Server&) = delete;
  Server& operator=(const Server&) = delete;

  bool init(std::string& err);
  // Starts the workers and blocks until shutdown. Returns the process exit code.
  int run();

  http::Router& router() noexcept { return router_; }
  const http::Router& router() const noexcept { return router_; }
  const Config& config() const noexcept { return cfg_; }
  int root_fd() const noexcept { return root_fd_.get(); }
  uint64_t per_worker_max_connections() const noexcept { return per_worker_max_; }
  bool draining() const noexcept { return draining_.load(std::memory_order_relaxed); }
  std::string metrics_text() const;

  // Called from worker threads.
  void on_worker_failed() noexcept { failed_.store(true); }
  void on_worker_exit() noexcept { running_.fetch_sub(1); }

 private:
  bool open_listeners(std::string& err);

  Config cfg_;
  http::Router router_;
  UniqueFd root_fd_;
  unsigned nworkers_ = 1;
  uint64_t per_worker_max_ = 0;
  bool shared_listeners_ = false;
  std::vector<UniqueFd> shared_fds_;
  std::vector<std::vector<int>> worker_fds_;
  std::vector<std::string> bound_;
  std::vector<std::unique_ptr<Worker>> workers_;
  std::atomic<bool> draining_{false};
  std::atomic<bool> failed_{false};
  std::atomic<int> running_{0};
  uint64_t start_ms_ = 0;
};

}  // namespace hws
