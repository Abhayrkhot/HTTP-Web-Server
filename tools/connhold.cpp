// hws-connhold: hold a very large number of concurrent keep-alive connections against an HTTP
// server, send one request per connection every --interval, and report how many stayed healthy,
// request latency percentiles and (optionally) the server's resident memory.
//
// Example (250k connections from 10 loopback source addresses):
//   hws-connhold --port 18080 --connections 250000 --sources 127.0.0.2-127.0.0.11
//                --interval 10000 --duration 60000 --threads 8 --server-pid $(pidof hws)
//
// Each source address gives ~28k ephemeral ports per destination, so use one source per 25k
// connections. IP_BIND_ADDRESS_NO_PORT defers port selection to connect() so the kernel can
// reuse ports across destinations.
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/epoll.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <vector>

#ifndef IP_BIND_ADDRESS_NO_PORT
#define IP_BIND_ADDRESS_NO_PORT 24
#endif

namespace {

uint64_t now_us() {
  timespec ts{};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<uint64_t>(ts.tv_sec) * 1000000u + static_cast<uint64_t>(ts.tv_nsec) / 1000u;
}

struct Options {
  std::string host = "127.0.0.1";
  uint16_t port = 8080;
  std::string path = "/hello";
  uint64_t connections = 10000;
  std::vector<in_addr> sources;
  uint64_t interval_ms = 5000;
  uint64_t duration_ms = 60000;
  uint64_t connect_rate = 20000;  // new connections per second, all threads
  unsigned threads = 4;
  std::vector<int> server_pids;  // RSS is summed across these (e.g. nginx workers)
  uint64_t timeout_ms = 5000;  // a response slower than this counts as an error
};

enum class State : uint8_t { Connecting, Idle, Waiting, Dead };

struct Conn {
  int fd = -1;
  State state = State::Connecting;
  uint64_t sent_us = 0;
  uint32_t body_left = 0;
  std::string head;  // response head bytes while waiting
};

struct Stats {
  std::atomic<uint64_t> connected{0};
  std::atomic<uint64_t> alive{0};
  std::atomic<uint64_t> connect_errors{0};
  std::atomic<uint64_t> requests{0};
  std::atomic<uint64_t> ok{0};
  std::atomic<uint64_t> non2xx{0};
  std::atomic<uint64_t> errors{0};  // resets, EOF, timeouts after connecting
};

Stats g_stats;
std::atomic<bool> g_stop{false};

void usage() {
  std::fprintf(stderr,
               "usage: hws-connhold [--host H] [--port P] [--path /x] [--connections N]\n"
               "                    [--sources A.B.C.D[-A.B.C.E][,..]] [--interval MS] [--duration MS]\n"
               "                    [--connect-rate N] [--threads N] [--server-pid PID[,PID..]] [--timeout MS]\n");
}

bool parse_sources(const std::string& spec, std::vector<in_addr>& out) {
  size_t start = 0;
  while (start <= spec.size()) {
    const size_t comma = spec.find(',', start);
    const std::string item = spec.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
    const size_t dash = item.find('-');
    in_addr a{};
    in_addr b{};
    if (inet_pton(AF_INET, item.substr(0, dash).c_str(), &a) != 1) return false;
    b = a;
    if (dash != std::string::npos && inet_pton(AF_INET, item.substr(dash + 1).c_str(), &b) != 1) return false;
    for (uint32_t x = ntohl(a.s_addr); x <= ntohl(b.s_addr); ++x) {
      in_addr v{};
      v.s_addr = htonl(x);
      out.push_back(v);
    }
    if (comma == std::string::npos) break;
    start = comma + 1;
  }
  return !out.empty();
}

uint64_t rss_kb_of(int pid) {
  if (pid <= 0) return 0;
  char path[64];
  std::snprintf(path, sizeof(path), "/proc/%d/status", pid);
  FILE* f = std::fopen(path, "r");
  if (f == nullptr) return 0;
  char line[256];
  uint64_t kb = 0;
  while (std::fgets(line, sizeof(line), f) != nullptr) {
    if (std::strncmp(line, "VmRSS:", 6) == 0) {
      kb = std::strtoull(line + 6, nullptr, 10);
      break;
    }
  }
  std::fclose(f);
  return kb;
}

uint64_t server_rss_kb(const std::vector<int>& pids) {
  uint64_t total = 0;
  for (int p : pids) total += rss_kb_of(p);
  return total;
}

class Worker {
 public:
  Worker(const Options& o, unsigned id, uint64_t count) : o_(o), id_(id), conns_(count), rng_(id * 7919 + 17) {
    epfd_ = epoll_create1(EPOLL_CLOEXEC);
    request_ = "GET " + o_.path + " HTTP/1.1\r\nHost: " + o_.host + "\r\n\r\n";
    std::memset(&dst_, 0, sizeof(dst_));
    dst_.sin_family = AF_INET;
    dst_.sin_port = htons(o_.port);
    inet_pton(AF_INET, o_.host.c_str(), &dst_.sin_addr);
    latencies_.reserve(1 << 20);
  }
  ~Worker() {
    for (auto& c : conns_) {
      if (c.fd >= 0) close(c.fd);
    }
    close(epfd_);
  }

  void run() {
    const uint64_t per_thread_rate = std::max<uint64_t>(1, o_.connect_rate / o_.threads);
    const uint64_t start = now_us();
    uint64_t next_connect = 0;
    std::vector<epoll_event> events(1024);
    while (!g_stop.load(std::memory_order_relaxed)) {
      const uint64_t now = now_us();
      // Ramp up connections at the configured rate.
      const uint64_t allowed = std::min<uint64_t>(conns_.size(), (now - start) * per_thread_rate / 1000000 + 1);
      while (next_connect < allowed) open_one(next_connect++);
      // Send due requests (FIFO is ordered because the interval is constant).
      while (!due_.empty() && due_.front().first <= now) {
        const size_t idx = due_.front().second;
        due_.pop_front();
        send_one(idx, now);
      }
      // Expire slow responses.
      while (!inflight_.empty() && inflight_.front().first + o_.timeout_ms * 1000 <= now) {
        const size_t idx = inflight_.front().second;
        inflight_.pop_front();
        Conn& c = conns_[idx];
        if (c.state == State::Waiting && c.sent_us + o_.timeout_ms * 1000 <= now) kill(idx, true);
      }
      const int n = epoll_wait(epfd_, events.data(), static_cast<int>(events.size()), 5);
      for (int i = 0; i < n; ++i) handle(events[static_cast<size_t>(i)].data.u64, events[static_cast<size_t>(i)].events);
    }
  }

  std::vector<uint32_t>& latencies() { return latencies_; }

 private:
  void open_one(size_t idx) {
    Conn& c = conns_[idx];
    c.fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (c.fd < 0) {
      g_stats.connect_errors.fetch_add(1);
      c.state = State::Dead;
      return;
    }
    const int one = 1;
    setsockopt(c.fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    if (!o_.sources.empty()) {
      setsockopt(c.fd, IPPROTO_IP, IP_BIND_ADDRESS_NO_PORT, &one, sizeof(one));
      sockaddr_in src{};
      src.sin_family = AF_INET;
      src.sin_addr = o_.sources[(idx * o_.threads + id_) % o_.sources.size()];
      if (bind(c.fd, reinterpret_cast<sockaddr*>(&src), sizeof(src)) != 0) {
        g_stats.connect_errors.fetch_add(1);
        close(c.fd);
        c.fd = -1;
        c.state = State::Dead;
        return;
      }
    }
    const int rc = connect(c.fd, reinterpret_cast<sockaddr*>(&dst_), sizeof(dst_));
    if (rc != 0 && errno != EINPROGRESS) {
      g_stats.connect_errors.fetch_add(1);
      close(c.fd);
      c.fd = -1;
      c.state = State::Dead;
      return;
    }
    epoll_event ev{};
    ev.events = EPOLLIN | EPOLLOUT | EPOLLRDHUP | EPOLLET;
    ev.data.u64 = idx;
    epoll_ctl(epfd_, EPOLL_CTL_ADD, c.fd, &ev);
  }

  void kill(size_t idx, bool error) {
    Conn& c = conns_[idx];
    if (c.state == State::Dead) return;
    const bool was_connected = c.state != State::Connecting;
    if (was_connected) g_stats.alive.fetch_sub(1);
    if (error) (was_connected ? g_stats.errors : g_stats.connect_errors).fetch_add(1);
    close(c.fd);
    c.fd = -1;
    c.state = State::Dead;
    std::string().swap(c.head);
  }

  void send_one(size_t idx, uint64_t now) {
    Conn& c = conns_[idx];
    if (c.state != State::Idle) return;
    const ssize_t n = send(c.fd, request_.data(), request_.size(), MSG_NOSIGNAL);
    if (n != static_cast<ssize_t>(request_.size())) {
      kill(idx, true);
      return;
    }
    c.state = State::Waiting;
    c.sent_us = now;
    inflight_.emplace_back(now, idx);
    g_stats.requests.fetch_add(1, std::memory_order_relaxed);
  }

  void handle(uint64_t idx, uint32_t ev) {
    Conn& c = conns_[idx];
    if (c.state == State::Dead) return;
    if (c.state == State::Connecting) {
      if (ev & (EPOLLERR | EPOLLHUP)) {
        kill(idx, true);
        return;
      }
      if (ev & EPOLLOUT) {
        int err = 0;
        socklen_t len = sizeof(err);
        getsockopt(c.fd, SOL_SOCKET, SO_ERROR, &err, &len);
        if (err != 0) {
          kill(idx, true);
          return;
        }
        c.state = State::Idle;
        g_stats.connected.fetch_add(1);
        g_stats.alive.fetch_add(1);
        std::uniform_int_distribution<uint64_t> jitter(0, o_.interval_ms * 1000);
        schedule(idx, now_us() + jitter(rng_));  // spread first requests across the interval
      }
      if (!(ev & EPOLLIN)) return;
    }
    if (ev & (EPOLLIN | EPOLLRDHUP | EPOLLHUP | EPOLLERR)) read_all(idx);
  }

  void schedule(size_t idx, uint64_t at) {
    if (!due_.empty() && at < due_.back().first) {
      // Initial jittered sends arrive out of order; insert in place (only during ramp-up).
      auto it = std::upper_bound(due_.begin(), due_.end(), std::make_pair(at, idx));
      due_.insert(it, {at, idx});
    } else {
      due_.emplace_back(at, idx);
    }
  }

  void read_all(size_t idx) {
    Conn& c = conns_[idx];
    char buf[4096];
    while (c.state != State::Dead) {
      const ssize_t n = recv(c.fd, buf, sizeof(buf), 0);
      if (n > 0) {
        consume(idx, buf, static_cast<size_t>(n));
        continue;
      }
      if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
      if (n < 0 && errno == EINTR) continue;
      kill(idx, true);  // EOF or reset while we expected to keep the connection
      return;
    }
  }

  void consume(size_t idx, const char* p, size_t n) {
    Conn& c = conns_[idx];
    while (n > 0 && c.state != State::Dead) {
      if (c.state != State::Waiting) {  // unsolicited bytes
        kill(idx, true);
        return;
      }
      if (c.body_left > 0) {
        const size_t take = std::min<size_t>(c.body_left, n);
        c.body_left -= static_cast<uint32_t>(take);
        p += take;
        n -= take;
        if (c.body_left == 0) complete(idx);
        continue;
      }
      c.head.append(p, n);
      const size_t end = c.head.find("\r\n\r\n");
      if (end == std::string::npos) {
        if (c.head.size() > 16384) kill(idx, true);
        return;
      }
      const int status = c.head.size() > 12 ? std::atoi(c.head.c_str() + 9) : 0;
      size_t clen = 0;
      const size_t cl = c.head.find("Content-Length:");
      if (cl != std::string::npos && cl < end) clen = std::strtoull(c.head.c_str() + cl + 15, nullptr, 10);
      const size_t extra = c.head.size() - (end + 4);
      std::string rest = c.head.substr(end + 4);
      c.head.clear();
      (status >= 200 && status < 300 ? g_stats.ok : g_stats.non2xx).fetch_add(1, std::memory_order_relaxed);
      if (extra >= clen) {
        complete(idx);
        if (extra > clen) consume(idx, rest.data() + clen, extra - clen);
        return;
      }
      c.body_left = static_cast<uint32_t>(clen - extra);
      return;
    }
  }

  void complete(size_t idx) {
    Conn& c = conns_[idx];
    const uint64_t now = now_us();
    latencies_.push_back(static_cast<uint32_t>(std::min<uint64_t>(now - c.sent_us, UINT32_MAX)));
    c.state = State::Idle;
    std::string().swap(c.head);
    due_.emplace_back(now + o_.interval_ms * 1000, idx);
  }

  const Options& o_;
  unsigned id_;
  int epfd_;
  std::vector<Conn> conns_;
  std::deque<std::pair<uint64_t, size_t>> due_;
  std::deque<std::pair<uint64_t, size_t>> inflight_;
  std::vector<uint32_t> latencies_;
  std::string request_;
  sockaddr_in dst_{};
  std::mt19937_64 rng_;
};

bool parse_u64(const char* s, uint64_t& out) {
  char* end = nullptr;
  errno = 0;
  const unsigned long long v = std::strtoull(s, &end, 10);
  if (errno != 0 || end == s || *end != '\0') return false;
  out = v;
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  Options o;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto next = [&]() -> const char* {
      if (i + 1 >= argc) {
        usage();
        std::exit(2);
      }
      return argv[++i];
    };
    uint64_t v = 0;
    if (a == "--host") o.host = next();
    else if (a == "--port" && parse_u64(next(), v)) o.port = static_cast<uint16_t>(v);
    else if (a == "--path") o.path = next();
    else if (a == "--connections" && parse_u64(next(), v)) o.connections = v;
    else if (a == "--sources") {
      if (!parse_sources(next(), o.sources)) {
        std::fprintf(stderr, "bad --sources\n");
        return 2;
      }
    } else if (a == "--interval" && parse_u64(next(), v)) o.interval_ms = std::max<uint64_t>(v, 1);
    else if (a == "--duration" && parse_u64(next(), v)) o.duration_ms = v;
    else if (a == "--connect-rate" && parse_u64(next(), v)) o.connect_rate = std::max<uint64_t>(v, 1);
    else if (a == "--threads" && parse_u64(next(), v)) o.threads = static_cast<unsigned>(std::max<uint64_t>(v, 1));
    else if (a == "--server-pid") {
      const std::string list = next();
      size_t start = 0;
      while (start < list.size()) {
        const size_t comma = list.find(',', start);
        const std::string item = list.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
        if (!parse_u64(item.c_str(), v)) {
          usage();
          return 2;
        }
        o.server_pids.push_back(static_cast<int>(v));
        if (comma == std::string::npos) break;
        start = comma + 1;
      }
    }
    else if (a == "--timeout" && parse_u64(next(), v)) o.timeout_ms = v;
    else {
      usage();
      return 2;
    }
  }

  rlimit rl{};
  getrlimit(RLIMIT_NOFILE, &rl);
  rl.rlim_cur = rl.rlim_max;
  setrlimit(RLIMIT_NOFILE, &rl);
  if (o.connections + 64 > rl.rlim_cur) {
    std::fprintf(stderr, "need %llu descriptors but RLIMIT_NOFILE is %llu; run several instances\n",
                 static_cast<unsigned long long>(o.connections + 64), static_cast<unsigned long long>(rl.rlim_cur));
    return 1;
  }

  const uint64_t rss_before = server_rss_kb(o.server_pids);
  std::vector<std::unique_ptr<Worker>> workers;
  std::vector<std::thread> threads;
  for (unsigned t = 0; t < o.threads; ++t) {
    const uint64_t n = o.connections / o.threads + (t < o.connections % o.threads ? 1 : 0);
    workers.push_back(std::make_unique<Worker>(o, t, n));
  }
  for (auto& w : workers) threads.emplace_back([&w] { w->run(); });

  const uint64_t start = now_us();
  uint64_t last_requests = 0;
  uint64_t peak_alive = 0;
  uint64_t rss_peak = 0;
  uint64_t rss_at_peak_alive = 0;
  std::printf("%6s %9s %9s %9s %10s %8s %8s %10s\n", "t(s)", "alive", "connected", "conn_err", "req/s", "non2xx",
              "errors", "srv_rss_mb");
  while (now_us() - start < o.duration_ms * 1000) {
    std::this_thread::sleep_for(std::chrono::seconds(1));
    const uint64_t req = g_stats.requests.load();
    const uint64_t alive = g_stats.alive.load();
    const uint64_t rss = server_rss_kb(o.server_pids);
    rss_peak = std::max(rss_peak, rss);
    if (alive >= peak_alive) {
      peak_alive = alive;
      rss_at_peak_alive = rss;
    }
    std::printf("%6.0f %9llu %9llu %9llu %10llu %8llu %8llu %10.1f\n", static_cast<double>(now_us() - start) / 1e6,
                static_cast<unsigned long long>(alive), static_cast<unsigned long long>(g_stats.connected.load()),
                static_cast<unsigned long long>(g_stats.connect_errors.load()),
                static_cast<unsigned long long>(req - last_requests),
                static_cast<unsigned long long>(g_stats.non2xx.load()),
                static_cast<unsigned long long>(g_stats.errors.load()), static_cast<double>(rss) / 1024.0);
    std::fflush(stdout);
    last_requests = req;
  }
  g_stop.store(true);
  for (auto& t : threads) t.join();

  std::vector<uint32_t> all;
  for (auto& w : workers) all.insert(all.end(), w->latencies().begin(), w->latencies().end());
  std::sort(all.begin(), all.end());
  auto pct = [&](double p) -> double {
    if (all.empty()) return 0;
    const size_t i = std::min(all.size() - 1, static_cast<size_t>(p * static_cast<double>(all.size())));
    return static_cast<double>(all[i]) / 1000.0;
  };
  const double per_conn_kb = peak_alive > 0 && rss_at_peak_alive > rss_before
                                 ? static_cast<double>(rss_at_peak_alive - rss_before) / static_cast<double>(peak_alive)
                                 : 0.0;
  std::printf(
      "\nSUMMARY target=%llu peak_alive=%llu final_alive=%llu connect_errors=%llu requests=%llu ok=%llu non2xx=%llu "
      "errors=%llu\n",
      static_cast<unsigned long long>(o.connections), static_cast<unsigned long long>(peak_alive),
      static_cast<unsigned long long>(g_stats.alive.load()),
      static_cast<unsigned long long>(g_stats.connect_errors.load()),
      static_cast<unsigned long long>(g_stats.requests.load()), static_cast<unsigned long long>(g_stats.ok.load()),
      static_cast<unsigned long long>(g_stats.non2xx.load()), static_cast<unsigned long long>(g_stats.errors.load()));
  std::printf("LATENCY_MS p50=%.3f p90=%.3f p99=%.3f p99.9=%.3f max=%.3f samples=%zu\n", pct(0.50), pct(0.90),
              pct(0.99), pct(0.999), all.empty() ? 0.0 : static_cast<double>(all.back()) / 1000.0, all.size());
  if (!o.server_pids.empty()) {
    std::printf("SERVER_RSS_MB before=%.1f at_peak=%.1f peak=%.1f per_conn_kb=%.2f\n",
                static_cast<double>(rss_before) / 1024.0, static_cast<double>(rss_at_peak_alive) / 1024.0,
                static_cast<double>(rss_peak) / 1024.0, per_conn_kb);
  }
  return g_stats.errors.load() == 0 && g_stats.non2xx.load() == 0 ? 0 : 1;
}
