#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "http/parser.h"
#include "http/response.h"

namespace hws::http {

// Handlers run synchronously on the worker thread that owns the connection. They must not block.
using Handler = std::function<void(const Request&, Response&)>;

constexpr uint16_t method_bit(Method m) noexcept { return static_cast<uint16_t>(1u << static_cast<unsigned>(m)); }
inline constexpr uint16_t kGet = method_bit(Method::Get);
inline constexpr uint16_t kPost = method_bit(Method::Post);
inline constexpr uint16_t kPut = method_bit(Method::Put);
inline constexpr uint16_t kDelete = method_bit(Method::Delete);

// Exact routes are a hash lookup; prefix routes are checked longest-first. Routes are registered
// before the server starts and are read-only afterwards, so lookups need no locking.
class Router {
 public:
  struct Route {
    std::string path;
    uint16_t methods = 0;  // GET implies HEAD
    bool prefix = false;
    Handler handler;
  };
  struct Match {
    const Route* route = nullptr;
    bool method_not_allowed = false;
  };

  void add(std::string path, uint16_t methods, Handler handler);
  void add_prefix(std::string prefix, uint16_t methods, Handler handler);
  Match find(std::string_view path, Method method) const;
  bool empty() const noexcept { return exact_.empty() && prefixes_.empty(); }

  // Value for an Allow header, e.g. "GET, HEAD, POST".
  static std::string allow_header(uint16_t methods);

 private:
  struct StringHash {
    using is_transparent = void;
    size_t operator()(std::string_view s) const noexcept { return std::hash<std::string_view>{}(s); }
  };

  std::unordered_map<std::string, Route, StringHash, std::equal_to<>> exact_;
  std::vector<Route> prefixes_;
};

}  // namespace hws::http
