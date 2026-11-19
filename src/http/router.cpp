#include "http/router.h"

#include <algorithm>

namespace hws::http {
namespace {

bool allows(uint16_t methods, Method m) noexcept {
  if (m == Method::Head) return (methods & (method_bit(Method::Head) | kGet)) != 0;
  return (methods & method_bit(m)) != 0;
}

}  // namespace

void Router::add(std::string path, uint16_t methods, Handler handler) {
  Route r{path, methods, false, std::move(handler)};
  exact_.insert_or_assign(std::move(path), std::move(r));
}

void Router::add_prefix(std::string prefix, uint16_t methods, Handler handler) {
  prefixes_.push_back(Route{std::move(prefix), methods, true, std::move(handler)});
  std::stable_sort(prefixes_.begin(), prefixes_.end(),
                   [](const Route& a, const Route& b) { return a.path.size() > b.path.size(); });
}

Router::Match Router::find(std::string_view path, Method method) const {
  Match m;
  if (auto it = exact_.find(path); it != exact_.end()) {
    m.route = &it->second;
    m.method_not_allowed = !allows(it->second.methods, method);
    return m;
  }
  for (const auto& r : prefixes_) {
    if (path.substr(0, r.path.size()) == r.path) {
      m.route = &r;
      m.method_not_allowed = !allows(r.methods, method);
      return m;
    }
  }
  return m;
}

std::string Router::allow_header(uint16_t methods) {
  std::string out;
  auto push = [&](Method m) {
    if (!out.empty()) out.append(", ");
    out.append(method_name(m));
  };
  if (methods & kGet) {
    push(Method::Get);
    push(Method::Head);
  } else if (methods & method_bit(Method::Head)) {
    push(Method::Head);
  }
  for (Method m : {Method::Post, Method::Put, Method::Delete, Method::Patch, Method::Options}) {
    if (methods & method_bit(m)) push(m);
  }
  return out;
}

}  // namespace hws::http
