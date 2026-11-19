#pragma once

#include <sys/socket.h>

#include <cstdint>
#include <string>
#include <string_view>

#include "util/fd.h"

namespace hws::net {

struct ListenAddress {
  std::string host;  // numeric IPv4/IPv6 or a resolvable name; "" means all interfaces
  uint16_t port = 0;
  std::string str() const;
};

// Accepts "host:port", "[v6]:port", ":port" and "port".
bool parse_listen_address(std::string_view text, ListenAddress& out, std::string& err);

struct ListenOptions {
  bool reuseport = true;
  int backlog = 65535;     // clamped by net.core.somaxconn
  int defer_accept_s = 0;  // TCP_DEFER_ACCEPT: wake only once data arrives
};

// Creates a non-blocking, close-on-exec listening socket.
UniqueFd open_listener(const ListenAddress& addr, const ListenOptions& opts, std::string& err);

uint16_t bound_port(int fd) noexcept;
bool set_nonblocking(int fd) noexcept;
void set_nodelay(int fd) noexcept;

// "1.2.3.4" / "::1" (IPv4-mapped IPv6 is shown as IPv4). Returns the length written (<= 46).
size_t format_ip(const sockaddr* sa, char* buf, size_t len) noexcept;

}  // namespace hws::net
