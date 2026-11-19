#include "server/access_log.h"

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>

#include "net/socket.h"
#include "util/log.h"

namespace hws {
namespace {

constexpr size_t kFlushBytes = 64 * 1024;

}  // namespace

bool AccessLog::open(const std::string& target, std::string& err) {
  path_ = target;
  if (target.empty() || target == "off") {
    fd_ = -1;
    return true;
  }
  if (target == "stdout" || target == "-") {
    fd_ = STDOUT_FILENO;
    return true;
  }
  UniqueFd fd{::open(target.c_str(), O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0644)};
  if (!fd) {
    err = "open access log " + target + ": " + std::strerror(errno);
    return false;
  }
  owned_ = std::move(fd);
  fd_ = owned_.get();
  return true;
}

void AccessLog::append_escaped(std::string_view s) {
  if (s.empty()) {
    buf_.append("-");
    return;
  }
  static constexpr char kHex[] = "0123456789abcdef";
  for (char c : s) {
    const auto u = static_cast<unsigned char>(c);
    if (c == '"' || c == '\\' || u < 0x20 || u >= 0x7f) {
      const char esc[4] = {'\\', 'x', kHex[u >> 4], kHex[u & 15]};
      buf_.append(esc, 4);
    } else {
      buf_.append(&c, 1);
    }
  }
}

void AccessLog::record(const sockaddr* peer, std::string_view time_local, const http::Request* req, uint16_t status,
                       uint64_t bytes, uint64_t micros) {
  if (fd_ < 0) return;
  char ip[64];
  buf_.append(ip, net::format_ip(peer, ip, sizeof(ip)));
  buf_.append(" - - [");
  buf_.append(time_local);
  buf_.append("] \"");
  if (req != nullptr) {
    append_escaped(req->method_token);
    buf_.append(" ");
    append_escaped(req->target);
    buf_.append(req->is_http10() ? " HTTP/1.0\" " : " HTTP/1.1\" ");
  } else {
    buf_.append("-\" ");
  }
  buf_.append_u64(status);
  buf_.append(" ");
  buf_.append_u64(bytes);
  buf_.append(" \"");
  append_escaped(req != nullptr ? req->referer : std::string_view{});
  buf_.append("\" \"");
  append_escaped(req != nullptr ? req->user_agent : std::string_view{});
  char t[32];
  const int n = std::snprintf(t, sizeof(t), "\" %llu.%06llu\n", static_cast<unsigned long long>(micros / 1000000),
                              static_cast<unsigned long long>(micros % 1000000));
  buf_.append(t, n > 0 ? static_cast<size_t>(n) : 0);
  if (buf_.size() >= kFlushBytes) flush();
}

void AccessLog::maybe_flush(uint64_t now_ms) {
  if (now_ms - last_flush_ms_ < 1000) return;
  flush();
  last_flush_ms_ = now_ms;
}

void AccessLog::flush() noexcept {
  while (!buf_.empty() && fd_ >= 0) {
    const ssize_t n = ::write(fd_, buf_.data(), buf_.size());
    if (n > 0) {
      buf_.consume(static_cast<size_t>(n));
    } else if (n < 0 && errno == EINTR) {
      continue;
    } else {
      buf_.clear();  // disk full or closed pipe: drop rather than grow without bound
      break;
    }
  }
}

void AccessLog::reopen() {
  if (!owned_) return;
  flush();
  UniqueFd fd{::open(path_.c_str(), O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0644)};
  if (!fd) {
    LOG_ERROR("reopen access log %s: %s", path_.c_str(), std::strerror(errno));
    return;
  }
  owned_ = std::move(fd);
  fd_ = owned_.get();
}

}  // namespace hws
