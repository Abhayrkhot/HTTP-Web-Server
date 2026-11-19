#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace hws {

// Single-writer counter: only the owning worker thread increments it, so a relaxed load+store
// is enough (no locked RMW on the hot path). Any thread may read it.
class Counter {
 public:
  void add(uint64_t n = 1) noexcept { v_.store(v_.load(std::memory_order_relaxed) + n, std::memory_order_relaxed); }
  uint64_t get() const noexcept { return v_.load(std::memory_order_relaxed); }

 private:
  std::atomic<uint64_t> v_{0};
};

// Fixed-bucket latency histogram in Prometheus layout.
class Histogram {
 public:
  static constexpr std::array<uint64_t, 17> kBoundsNs = {
      50'000,     100'000,     250'000,     500'000,       1'000'000,     2'500'000,
      5'000'000,  10'000'000,  25'000'000,  50'000'000,    100'000'000,   250'000'000,
      500'000'000, 1'000'000'000, 2'500'000'000, 5'000'000'000, 10'000'000'000};

  void observe(uint64_t ns, uint64_t n = 1) noexcept {
    size_t i = 0;
    while (i < kBoundsNs.size() && ns > kBoundsNs[i]) ++i;
    buckets_[i].add(n);
    sum_ns_.add(ns * n);
    count_.add(n);
  }
  uint64_t bucket(size_t i) const noexcept { return buckets_[i].get(); }
  uint64_t sum_ns() const noexcept { return sum_ns_.get(); }
  uint64_t count() const noexcept { return count_.get(); }

 private:
  std::array<Counter, kBoundsNs.size() + 1> buckets_{};
  Counter sum_ns_;
  Counter count_;
};

struct alignas(64) WorkerMetrics {
  Counter accepted;
  Counter closed;
  Counter rejected_limit;  // over max-connections: answered 503
  Counter rejected_fd;     // EMFILE/ENFILE: shed with the reserve descriptor
  std::array<Counter, 6> responses;  // index = status / 100 (1..5)
  Counter bytes_in;
  Counter bytes_out;
  Counter timeout_header;
  Counter timeout_body;
  Counter timeout_idle;
  Counter timeout_write;
  Counter parse_errors;
  Counter handler_errors;
  Counter file_cache_hits;
  Counter file_cache_misses;
  Histogram latency;  // request fully received -> response accepted by the kernel

  uint64_t active() const noexcept { return accepted.get() - closed.get(); }
};

struct ProcessInfo {
  bool draining = false;
  uint64_t uptime_s = 0;
  unsigned workers = 0;
};

std::string render_prometheus(const std::vector<const WorkerMetrics*>& workers, const ProcessInfo& info);

// Resident set size of this process in bytes (from /proc/self/statm); 0 if unavailable.
uint64_t resident_memory_bytes() noexcept;

}  // namespace hws
