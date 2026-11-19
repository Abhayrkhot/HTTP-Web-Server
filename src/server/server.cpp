#include "server/server.h"

#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <sys/resource.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <thread>

#include "net/socket.h"
#include "server/access_log.h"
#include "server/metrics.h"
#include "server/worker.h"
#include "util/clock.h"
#include "util/log.h"
#include "util/strings.h"

namespace hws {
namespace {

std::vector<unsigned> allowed_cpus() {
  std::vector<unsigned> cpus;
  cpu_set_t set;
  CPU_ZERO(&set);
  if (sched_getaffinity(0, sizeof(set), &set) == 0) {
    for (unsigned i = 0; i < CPU_SETSIZE; ++i) {
      if (CPU_ISSET(i, &set)) cpus.push_back(i);
    }
  }
  if (cpus.empty()) {
    const unsigned n = std::max(1u, std::thread::hardware_concurrency());
    for (unsigned i = 0; i < n; ++i) cpus.push_back(i);
  }
  return cpus;
}

// systemd socket activation (sd_listen_fds(3)): descriptors start at 3.
std::vector<int> inherited_listeners() {
  std::vector<int> fds;
  const char* pid = std::getenv("LISTEN_PID");
  const char* count = std::getenv("LISTEN_FDS");
  uint64_t p = 0;
  uint64_t n = 0;
  if (pid == nullptr || count == nullptr || !parse_u64(pid, p) || !parse_u64(count, n)) return fds;
  if (p != static_cast<uint64_t>(getpid())) return fds;
  for (uint64_t i = 0; i < n; ++i) {
    const int fd = 3 + static_cast<int>(i);
    ::fcntl(fd, F_SETFD, FD_CLOEXEC);
    net::set_nonblocking(fd);
    fds.push_back(fd);
  }
  unsetenv("LISTEN_PID");
  unsetenv("LISTEN_FDS");
  unsetenv("LISTEN_FDNAMES");
  return fds;
}

}  // namespace

Server::Server(Config cfg) : cfg_(std::move(cfg)) {}

Server::~Server() {
  workers_.clear();  // joins threads before listeners are closed
}

bool Server::init(std::string& err) {
  start_ms_ = monotonic_ms();
  log::Level level{};
  log::parse_level(cfg_.log_level, level);
  log::set_level(level);

  const std::vector<unsigned> cpus = allowed_cpus();
  nworkers_ = cfg_.workers != 0 ? cfg_.workers : static_cast<unsigned>(cpus.size());

  // Connection capacity is bounded by descriptors: raise the soft limit to the hard limit.
  rlimit rl{};
  if (getrlimit(RLIMIT_NOFILE, &rl) == 0 && rl.rlim_cur < rl.rlim_max) {
    rl.rlim_cur = rl.rlim_max;
    setrlimit(RLIMIT_NOFILE, &rl);
  }
  getrlimit(RLIMIT_NOFILE, &rl);
  const uint64_t nofile = rl.rlim_cur;
  uint64_t reserve = 64 + 8ull * nworkers_;
  if (!cfg_.root.empty()) reserve += std::min<uint64_t>(cfg_.file_cache_entries * nworkers_, nofile / 4);
  const uint64_t fd_cap = nofile > reserve + nworkers_ ? nofile - reserve : nworkers_;
  uint64_t max_conns = cfg_.max_connections != 0 ? cfg_.max_connections : fd_cap;
  if (max_conns > fd_cap) {
    LOG_WARN("max-connections %llu exceeds what RLIMIT_NOFILE=%llu allows; using %llu",
             static_cast<unsigned long long>(max_conns), static_cast<unsigned long long>(nofile),
             static_cast<unsigned long long>(fd_cap));
    max_conns = fd_cap;
  }
  per_worker_max_ = std::max<uint64_t>(1, max_conns / nworkers_);

  if (!cfg_.root.empty()) {
    root_fd_.reset(::open(cfg_.root.c_str(), O_PATH | O_DIRECTORY | O_CLOEXEC));
    if (!root_fd_) {
      err = "open root " + cfg_.root + ": " + std::strerror(errno);
      return false;
    }
  }
  {
    AccessLog probe;
    if (!probe.open(cfg_.access_log, err)) return false;
  }
  if (!open_listeners(err)) return false;

  for (unsigned i = 0; i < nworkers_; ++i) {
    std::vector<int> fds;
    if (shared_listeners_) {
      for (const auto& fd : shared_fds_) fds.push_back(fd.get());
    } else {
      fds = std::move(worker_fds_[i]);
    }
    workers_.push_back(std::make_unique<Worker>(*this, i, std::move(fds), !shared_listeners_));
  }
  LOG_INFO("hws %s: %u workers, %s listeners, max %llu connections (RLIMIT_NOFILE %llu)", HWS_VERSION, nworkers_,
           shared_listeners_ ? "shared" : "SO_REUSEPORT", static_cast<unsigned long long>(per_worker_max_ * nworkers_),
           static_cast<unsigned long long>(nofile));
  return true;
}

bool Server::open_listeners(std::string& err) {
  if (std::vector<int> inherited = inherited_listeners(); !inherited.empty()) {
    shared_listeners_ = true;
    for (int fd : inherited) {
      shared_fds_.emplace_back(fd);
      bound_.push_back("fd:" + std::to_string(fd) + " port " + std::to_string(net::bound_port(fd)));
    }
    return true;
  }
  net::ListenOptions opts;
  opts.reuseport = cfg_.reuseport;
  opts.backlog = cfg_.backlog;
  opts.defer_accept_s = cfg_.defer_accept_s;
  shared_listeners_ = !cfg_.reuseport;
  worker_fds_.assign(nworkers_, {});
  for (const std::string& text : cfg_.listen) {
    net::ListenAddress addr;
    if (!net::parse_listen_address(text, addr, err)) return false;
    const unsigned copies = shared_listeners_ ? 1 : nworkers_;
    for (unsigned i = 0; i < copies; ++i) {
      UniqueFd fd = net::open_listener(addr, opts, err);
      if (!fd) return false;
      if (addr.port == 0) addr.port = net::bound_port(fd.get());  // later copies join the same port
      if (shared_listeners_) {
        shared_fds_.push_back(std::move(fd));
      } else {
        worker_fds_[i].push_back(fd.release());
      }
    }
    bound_.push_back(addr.str());
  }
  return true;
}

int Server::run() {
  // Block lifecycle signals in every thread; the main thread consumes them with sigtimedwait.
  sigset_t set;
  sigemptyset(&set);
  sigaddset(&set, SIGINT);
  sigaddset(&set, SIGTERM);
  sigaddset(&set, SIGHUP);
  sigaddset(&set, SIGQUIT);
  pthread_sigmask(SIG_BLOCK, &set, nullptr);

  const std::vector<unsigned> cpus = allowed_cpus();
  running_.store(static_cast<int>(workers_.size()));
  for (size_t i = 0; i < workers_.size(); ++i) {
    std::optional<unsigned> cpu;
    if (cfg_.pin_workers) cpu = cpus[i % cpus.size()];
    workers_[i]->start(cpu);
  }
  for (const auto& b : bound_) LOG_INFO("listening on %s", b.c_str());

  bool stopping = false;
  while (running_.load() > 0) {
    if (failed_.load() && !stopping) {
      LOG_ERROR("a worker failed; stopping");
      for (auto& w : workers_) w->stop();
      stopping = true;
    }
    siginfo_t info{};
    timespec ts{0, 200 * 1000 * 1000};
    const int sig = sigtimedwait(&set, &info, &ts);
    if (sig < 0) continue;
    if (sig == SIGHUP) {
      LOG_INFO("SIGHUP: reopening logs");
      for (auto& w : workers_) w->reopen_logs();
    } else if ((sig == SIGTERM || sig == SIGINT) && !draining_.load()) {
      LOG_INFO("%s: draining connections (deadline %llums)", sig == SIGTERM ? "SIGTERM" : "SIGINT",
               static_cast<unsigned long long>(cfg_.drain_timeout_ms));
      draining_.store(true);
      for (auto& w : workers_) w->drain(cfg_.drain_timeout_ms);
    } else if (!stopping) {
      LOG_WARN("%s: stopping now", strsignal(sig));
      for (auto& w : workers_) w->stop();
      stopping = true;
    }
  }
  for (auto& w : workers_) w->join();
  LOG_INFO("shutdown complete");
  return failed_.load() ? 1 : 0;
}

std::string Server::metrics_text() const {
  std::vector<const WorkerMetrics*> ms;
  ms.reserve(workers_.size());
  for (const auto& w : workers_) ms.push_back(&w->metrics());
  ProcessInfo info;
  info.draining = draining();
  info.uptime_s = (monotonic_ms() - start_ms_) / 1000;
  info.workers = nworkers_;
  return render_prometheus(ms, info);
}

}  // namespace hws
