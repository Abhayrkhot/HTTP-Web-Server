#pragma once

#include <sys/socket.h>

#include <cstdint>
#include <string>
#include <string_view>

#include "http/parser.h"
#include "util/buffer.h"
#include "util/fd.h"

namespace hws {

// Per-worker buffered access log in Combined Log Format plus request time. Each worker holds its
// own O_APPEND descriptor and writes whole lines in batches (64 KiB or every second), so workers
// never contend and lines never interleave.
class AccessLog {
 public:
  // target: "off", "stdout", or a file path.
  bool open(const std::string& target, std::string& err);
  bool enabled() const noexcept { return fd_ >= 0; }

  void record(const sockaddr* peer, std::string_view time_local, const http::Request* req, uint16_t status,
              uint64_t bytes, uint64_t micros);
  void maybe_flush(uint64_t now_ms);
  void flush() noexcept;
  // Re-opens the file (log rotation). Keeps the old descriptor if the open fails.
  void reopen();

 private:
  void append_escaped(std::string_view s);

  std::string path_;
  UniqueFd owned_;
  int fd_ = -1;
  Buffer buf_;
  uint64_t last_flush_ms_ = 0;
};

}  // namespace hws
