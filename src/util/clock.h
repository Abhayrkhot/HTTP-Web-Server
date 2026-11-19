#pragma once

#include <cstdint>
#include <ctime>

namespace hws {

// Coarse monotonic milliseconds (vDSO, ~1-4 ms resolution). Used for timeouts.
inline uint64_t monotonic_ms() noexcept {
  timespec ts{};
  clock_gettime(CLOCK_MONOTONIC_COARSE, &ts);
  return static_cast<uint64_t>(ts.tv_sec) * 1000u + static_cast<uint64_t>(ts.tv_nsec) / 1000000u;
}

// Precise monotonic nanoseconds. Used for latency measurement.
inline uint64_t monotonic_ns() noexcept {
  timespec ts{};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<uint64_t>(ts.tv_sec) * 1000000000u + static_cast<uint64_t>(ts.tv_nsec);
}

inline time_t realtime_sec() noexcept {
  timespec ts{};
  clock_gettime(CLOCK_REALTIME_COARSE, &ts);
  return ts.tv_sec;
}

}  // namespace hws
