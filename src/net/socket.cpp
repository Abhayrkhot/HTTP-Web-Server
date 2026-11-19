#include "net/socket.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

#include <cerrno>
#include <cstring>

#include "util/strings.h"

namespace hws::net {

std::string ListenAddress::str() const {
  const bool v6 = host.find(':') != std::string::npos;
  std::string h = host.empty() ? "0.0.0.0" : host;
  if (v6) h = "[" + h + "]";
  return h + ":" + std::to_string(port);
}

bool parse_listen_address(std::string_view text, ListenAddress& out, std::string& err) {
  std::string_view host;
  std::string_view port;
  if (!text.empty() && text.front() == '[') {
    const size_t close = text.find(']');
    if (close == std::string_view::npos || close + 1 >= text.size() || text[close + 1] != ':') {
      err = "expected [ipv6]:port";
      return false;
    }
    host = text.substr(1, close - 1);
    port = text.substr(close + 2);
  } else if (const size_t colon = text.rfind(':'); colon != std::string_view::npos) {
    host = text.substr(0, colon);
    port = text.substr(colon + 1);
    if (host.find(':') != std::string_view::npos) {
      err = "IPv6 addresses must be bracketed, e.g. [::]:8080";
      return false;
    }
  } else {
    port = text;
  }
  uint64_t p = 0;
  if (!parse_u64(port, p) || p > 65535) {
    err = "invalid port in listen address '" + std::string(text) + "'";
    return false;
  }
  out.host = std::string(host);
  out.port = static_cast<uint16_t>(p);
  return true;
}

UniqueFd open_listener(const ListenAddress& addr, const ListenOptions& opts, std::string& err) {
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = AI_PASSIVE | AI_NUMERICSERV;
  addrinfo* res = nullptr;
  const std::string port = std::to_string(addr.port);
  const int rc = getaddrinfo(addr.host.empty() ? nullptr : addr.host.c_str(), port.c_str(), &hints, &res);
  if (rc != 0) {
    err = "resolve " + addr.str() + ": " + gai_strerror(rc);
    return UniqueFd{};
  }
  // Prefer IPv4 for an empty host so "0.0.0.0" semantics match the default config.
  addrinfo* pick = res;
  if (addr.host.empty()) {
    for (addrinfo* ai = res; ai != nullptr; ai = ai->ai_next) {
      if (ai->ai_family == AF_INET) {
        pick = ai;
        break;
      }
    }
  }
  UniqueFd fd{::socket(pick->ai_family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0)};
  if (!fd) {
    err = std::string("socket: ") + std::strerror(errno);
    freeaddrinfo(res);
    return UniqueFd{};
  }
  const int one = 1;
  ::setsockopt(fd.get(), SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  if (opts.reuseport && ::setsockopt(fd.get(), SOL_SOCKET, SO_REUSEPORT, &one, sizeof(one)) != 0) {
    err = std::string("SO_REUSEPORT: ") + std::strerror(errno);
    freeaddrinfo(res);
    return UniqueFd{};
  }
  if (::bind(fd.get(), pick->ai_addr, pick->ai_addrlen) != 0) {
    err = "bind " + addr.str() + ": " + std::strerror(errno);
    freeaddrinfo(res);
    return UniqueFd{};
  }
  freeaddrinfo(res);
  if (::listen(fd.get(), opts.backlog) != 0) {
    err = "listen " + addr.str() + ": " + std::strerror(errno);
    return UniqueFd{};
  }
  if (opts.defer_accept_s > 0) {
    ::setsockopt(fd.get(), IPPROTO_TCP, TCP_DEFER_ACCEPT, &opts.defer_accept_s, sizeof(opts.defer_accept_s));
  }
  return fd;
}

uint16_t bound_port(int fd) noexcept {
  sockaddr_storage ss{};
  socklen_t len = sizeof(ss);
  if (::getsockname(fd, reinterpret_cast<sockaddr*>(&ss), &len) != 0) return 0;
  if (ss.ss_family == AF_INET) return ntohs(reinterpret_cast<sockaddr_in*>(&ss)->sin_port);
  if (ss.ss_family == AF_INET6) return ntohs(reinterpret_cast<sockaddr_in6*>(&ss)->sin6_port);
  return 0;
}

bool set_nonblocking(int fd) noexcept {
  const int flags = ::fcntl(fd, F_GETFL);
  return flags >= 0 && ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

void set_nodelay(int fd) noexcept {
  const int one = 1;
  ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
}

size_t format_ip(const sockaddr* sa, char* buf, size_t len) noexcept {
  const char* r = nullptr;
  if (sa->sa_family == AF_INET) {
    r = ::inet_ntop(AF_INET, &reinterpret_cast<const sockaddr_in*>(sa)->sin_addr, buf, static_cast<socklen_t>(len));
  } else if (sa->sa_family == AF_INET6) {
    const auto* a6 = reinterpret_cast<const sockaddr_in6*>(sa);
    if (IN6_IS_ADDR_V4MAPPED(&a6->sin6_addr)) {
      r = ::inet_ntop(AF_INET, &a6->sin6_addr.s6_addr[12], buf, static_cast<socklen_t>(len));
    } else {
      r = ::inet_ntop(AF_INET6, &a6->sin6_addr, buf, static_cast<socklen_t>(len));
    }
  }
  if (r == nullptr) {
    std::strncpy(buf, "-", len);
    return 1;
  }
  return std::strlen(buf);
}

}  // namespace hws::net
