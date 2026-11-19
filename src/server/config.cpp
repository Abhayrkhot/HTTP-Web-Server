#include "server/config.h"

#include <cstdio>
#include <fstream>
#include <functional>
#include <sstream>

#include "net/socket.h"
#include "util/log.h"
#include "util/strings.h"

namespace hws {
namespace {

using Setter = std::function<bool(Config&, std::string_view, std::string&)>;

struct Option {
  const char* name;
  const char* arg;  // nullptr for boolean flags
  const char* help;
  Setter set;
};

bool parse_bool(std::string_view v, bool& out) noexcept {
  if (v.empty() || iequals(v, "on") || iequals(v, "true") || iequals(v, "yes") || v == "1") {
    out = true;
  } else if (iequals(v, "off") || iequals(v, "false") || iequals(v, "no") || v == "0") {
    out = false;
  } else {
    return false;
  }
  return true;
}

Setter str(std::string Config::* field) {
  return [field](Config& c, std::string_view v, std::string&) {
    c.*field = std::string(v);
    return true;
  };
}

Setter boolean(bool Config::* field) {
  return [field](Config& c, std::string_view v, std::string& err) {
    if (parse_bool(v, c.*field)) return true;
    err = "expected on/off";
    return false;
  };
}

Setter duration(uint64_t Config::* field) {
  return [field](Config& c, std::string_view v, std::string& err) {
    if (parse_duration_ms(v, c.*field)) return true;
    err = "expected a duration like 500ms, 10s or 2m";
    return false;
  };
}

Setter size(uint64_t Config::* field) {
  return [field](Config& c, std::string_view v, std::string& err) {
    if (parse_size(v, c.*field)) return true;
    err = "expected a size like 512, 16k or 1m";
    return false;
  };
}

Setter count(uint64_t Config::* field) {
  return [field](Config& c, std::string_view v, std::string& err) {
    if (parse_u64(v, c.*field)) return true;
    err = "expected a non-negative integer";
    return false;
  };
}

const std::vector<Option>& options() {
  static const std::vector<Option> opts = {
      {"listen", "ADDR[,ADDR...]", "addresses to listen on (default 0.0.0.0:8080; [::]:8080 for IPv6)",
       [](Config& c, std::string_view v, std::string& err) {
         c.listen.clear();
         bool ok = true;
         for_each_list_item(v, [&](std::string_view a) {
           net::ListenAddress la;
           if (!net::parse_listen_address(a, la, err)) ok = false;
           c.listen.emplace_back(a);
         });
         if (c.listen.empty()) {
           err = "at least one address is required";
           return false;
         }
         return ok;
       }},
      {"workers", "N", "worker threads, each with its own epoll loop (default: online CPUs)",
       [](Config& c, std::string_view v, std::string& err) {
         uint64_t n = 0;
         if (!parse_u64(v, n) || n > 1024) {
           err = "expected 0..1024";
           return false;
         }
         c.workers = static_cast<unsigned>(n);
         return true;
       }},
      {"reuseport", "on|off", "one SO_REUSEPORT listener per worker (default on)", boolean(&Config::reuseport)},
      {"backlog", "N", "listen backlog (default 65535, capped by net.core.somaxconn)",
       [](Config& c, std::string_view v, std::string& err) {
         uint64_t n = 0;
         if (!parse_u64(v, n) || n == 0 || n > 1u << 20) {
           err = "expected 1..1048576";
           return false;
         }
         c.backlog = static_cast<int>(n);
         return true;
       }},
      {"defer-accept", "SECONDS", "TCP_DEFER_ACCEPT: accept only once the client sends data (default 0 = off)",
       [](Config& c, std::string_view v, std::string& err) {
         uint64_t n = 0;
         if (!parse_u64(v, n) || n > 3600) {
           err = "expected 0..3600";
           return false;
         }
         c.defer_accept_s = static_cast<int>(n);
         return true;
       }},
      {"pin-workers", nullptr, "pin each worker thread to one CPU", boolean(&Config::pin_workers)},
      {"max-connections", "N", "total concurrent connections (default: from RLIMIT_NOFILE)",
       count(&Config::max_connections)},
      {"root", "DIR", "serve static files from DIR (default: disabled)", str(&Config::root)},
      {"index", "NAME", "directory index file (default index.html)", str(&Config::index)},
      {"serve-dotfiles", nullptr, "serve paths with segments starting with '.' (default off)",
       boolean(&Config::serve_dotfiles)},
      {"follow-symlinks", "on|off", "follow symlinks that stay inside root (default on)",
       boolean(&Config::follow_symlinks)},
      {"cache-control", "VALUE", "Cache-Control header for static files (default: none)",
       str(&Config::cache_control)},
      {"file-cache-entries", "N", "open-file cache entries per worker (default 1024)",
       count(&Config::file_cache_entries)},
      {"file-cache-ttl", "DURATION", "revalidate cached file metadata after this long (default 2s)",
       duration(&Config::file_cache_ttl_ms)},
      {"inline-file-max", "SIZE", "serve files up to SIZE from memory instead of sendfile (default 16k)",
       size(&Config::inline_file_max)},
      {"inline-cache-bytes", "SIZE", "per-worker memory budget for in-memory files (default 32m)",
       size(&Config::inline_cache_bytes)},
      {"keepalive-timeout", "DURATION", "close idle keep-alive connections after (default 60s)",
       duration(&Config::keepalive_timeout_ms)},
      {"header-timeout", "DURATION", "time allowed to receive a full request head (default 10s)",
       duration(&Config::header_timeout_ms)},
      {"body-timeout", "DURATION", "time allowed to receive a request body (default 30s)",
       duration(&Config::body_timeout_ms)},
      {"write-timeout", "DURATION", "close if the client accepts no response bytes for (default 30s)",
       duration(&Config::write_timeout_ms)},
      {"linger-timeout", "DURATION", "drain time for unread input before closing (default 2s)",
       duration(&Config::linger_timeout_ms)},
      {"drain-timeout", "DURATION", "graceful shutdown deadline (default 30s)", duration(&Config::drain_timeout_ms)},
      {"max-header-bytes", "SIZE", "request line + headers limit, 431 beyond (default 16k)",
       size(&Config::max_header_bytes)},
      {"max-headers", "N", "header field count limit (default 100, max 256)", count(&Config::max_headers)},
      {"max-uri-bytes", "SIZE", "request-target limit, 414 beyond (default 8k)", size(&Config::max_uri_bytes)},
      {"max-body-bytes", "SIZE", "request body limit, 413 beyond (default 1m)", size(&Config::max_body_bytes)},
      {"max-requests-per-connection", "N", "close keep-alive connections after N requests (default 10000, 0 = no limit)",
       count(&Config::max_requests_per_connection)},
      {"access-log", "off|stdout|FILE", "combined-format access log (default off)", str(&Config::access_log)},
      {"log-level", "LEVEL", "debug|info|warn|error|off (default info)", str(&Config::log_level)},
      {"server-name", "NAME", "Server header value (default hws)", str(&Config::server_name)},
      {"health-path", "PATH", "health endpoint, 503 while draining (default /healthz; empty disables)",
       str(&Config::health_path)},
      {"metrics-path", "PATH", "Prometheus metrics endpoint (default /metrics; empty disables)",
       str(&Config::metrics_path)},
      {"hello-path", "PATH", "plaintext Hello, World! endpoint for benchmarks (default: disabled)",
       str(&Config::hello_path)},
      {"echo-path", "PATH", "POST endpoint that echoes the request body (default: disabled)",
       str(&Config::echo_path)},
  };
  return opts;
}

const Option* find_option(std::string_view name) {
  for (const auto& o : options()) {
    if (name == o.name) return &o;
  }
  return nullptr;
}

bool apply(Config& cfg, std::string_view name, std::string_view value, std::string& err) {
  const Option* o = find_option(name);
  if (o == nullptr) {
    err = "unknown option '" + std::string(name) + "'";
    return false;
  }
  std::string why;
  if (!o->set(cfg, value, why)) {
    err = "invalid value '" + std::string(value) + "' for " + std::string(name) + ": " + why;
    return false;
  }
  return true;
}

}  // namespace

bool parse_duration_ms(std::string_view text, uint64_t& out) noexcept {
  text = trim_ows(text);
  size_t i = 0;
  while (i < text.size() && text[i] >= '0' && text[i] <= '9') ++i;
  uint64_t n = 0;
  if (!parse_u64(text.substr(0, i), n)) return false;
  const std::string_view unit = text.substr(i);
  uint64_t mul = 0;
  if (unit.empty() || unit == "ms") mul = 1;
  else if (unit == "s") mul = 1000;
  else if (unit == "m") mul = 60'000;
  else if (unit == "h") mul = 3'600'000;
  else return false;
  if (n > UINT64_MAX / mul) return false;
  out = n * mul;
  return true;
}

bool parse_size(std::string_view text, uint64_t& out) noexcept {
  text = trim_ows(text);
  size_t i = 0;
  while (i < text.size() && text[i] >= '0' && text[i] <= '9') ++i;
  uint64_t n = 0;
  if (!parse_u64(text.substr(0, i), n)) return false;
  const std::string_view unit = text.substr(i);
  uint64_t mul = 0;
  if (unit.empty() || unit == "b") mul = 1;
  else if (iequals(unit, "k") || iequals(unit, "kb") || iequals(unit, "kib")) mul = 1ull << 10;
  else if (iequals(unit, "m") || iequals(unit, "mb") || iequals(unit, "mib")) mul = 1ull << 20;
  else if (iequals(unit, "g") || iequals(unit, "gb") || iequals(unit, "gib")) mul = 1ull << 30;
  else return false;
  if (n > UINT64_MAX / mul) return false;
  out = n * mul;
  return true;
}

bool load_config_file(const std::string& path, Config& cfg, std::string& err) {
  std::ifstream in(path);
  if (!in) {
    err = "cannot open config file " + path;
    return false;
  }
  std::string line;
  int lineno = 0;
  while (std::getline(in, line)) {
    ++lineno;
    std::string_view l = trim_ows(line);
    if (const size_t hash = l.find('#'); hash != std::string_view::npos) l = trim_ows(l.substr(0, hash));
    if (l.empty()) continue;
    const size_t eq = l.find('=');
    const std::string_view name = trim_ows(l.substr(0, eq));
    const std::string_view value = eq == std::string_view::npos ? std::string_view{} : trim_ows(l.substr(eq + 1));
    std::string why;
    if (!apply(cfg, name, value, why)) {
      err = path + ":" + std::to_string(lineno) + ": " + why;
      return false;
    }
  }
  return true;
}

ArgsResult parse_args(int argc, char** argv, Config& cfg, std::string& err) {
  std::vector<std::string_view> args(argv + 1, argv + argc);
  // Pass 1: --config, --help, --version.
  for (size_t i = 0; i < args.size(); ++i) {
    const std::string_view a = args[i];
    if (a == "-h" || a == "--help") {
      std::fputs(usage().c_str(), stdout);
      return ArgsResult::Exit;
    }
    if (a == "-v" || a == "--version") {
      std::printf("hws %s\n", HWS_VERSION);
      return ArgsResult::Exit;
    }
    if (a == "--config" || a.substr(0, 9) == "--config=") {
      std::string path;
      if (a == "--config") {
        if (i + 1 >= args.size()) {
          err = "--config needs a file";
          return ArgsResult::Error;
        }
        path = std::string(args[++i]);
      } else {
        path = std::string(a.substr(9));
      }
      if (!load_config_file(path, cfg, err)) return ArgsResult::Error;
    }
  }
  // Pass 2: everything else.
  for (size_t i = 0; i < args.size(); ++i) {
    std::string_view a = args[i];
    if (a == "--config") {
      ++i;
      continue;
    }
    if (a.substr(0, 9) == "--config=") continue;
    if (a.substr(0, 2) != "--") {
      err = "unexpected argument '" + std::string(a) + "'";
      return ArgsResult::Error;
    }
    a.remove_prefix(2);
    std::string_view name = a;
    std::string_view value;
    bool have_value = false;
    if (const size_t eq = a.find('='); eq != std::string_view::npos) {
      name = a.substr(0, eq);
      value = a.substr(eq + 1);
      have_value = true;
    }
    const Option* o = find_option(name);
    if (o == nullptr) {
      err = "unknown option '--" + std::string(name) + "' (see --help)";
      return ArgsResult::Error;
    }
    if (!have_value && o->arg != nullptr) {
      if (i + 1 >= args.size()) {
        err = "--" + std::string(name) + " needs a value";
        return ArgsResult::Error;
      }
      value = args[++i];
    }
    if (!apply(cfg, name, value, err)) return ArgsResult::Error;
  }
  return ArgsResult::Run;
}

bool validate_config(Config& cfg, std::string& err) {
  log::Level level{};
  if (!log::parse_level(cfg.log_level, level)) {
    err = "invalid log-level '" + cfg.log_level + "'";
    return false;
  }
  if (cfg.max_headers == 0 || cfg.max_headers > 256) {
    err = "max-headers must be 1..256";
    return false;
  }
  if (cfg.max_header_bytes < 256) {
    err = "max-header-bytes must be at least 256";
    return false;
  }
  if (cfg.keepalive_timeout_ms == 0 || cfg.header_timeout_ms == 0 || cfg.body_timeout_ms == 0 ||
      cfg.write_timeout_ms == 0) {
    err = "timeouts must be positive";
    return false;
  }
  for (const std::string* p : {&cfg.health_path, &cfg.metrics_path, &cfg.hello_path, &cfg.echo_path}) {
    if (!p->empty() && (*p)[0] != '/') {
      err = "endpoint paths must start with '/': " + *p;
      return false;
    }
  }
  for (char c : cfg.server_name) {
    if (c < 0x21 || c > 0x7e) {
      err = "server-name must be visible ASCII";
      return false;
    }
  }
  if (cfg.index.find('/') != std::string::npos || cfg.index.empty()) {
    err = "index must be a plain file name";
    return false;
  }
  return true;
}

std::string usage() {
  std::ostringstream os;
  os << "hws " << HWS_VERSION << " - high-performance HTTP/1.1 server\n\n"
     << "usage: hws [--config FILE] [options]\n\n"
     << "Options can also be given in a config file as 'name = value' lines.\n"
     << "Durations accept ms/s/m/h suffixes; sizes accept k/m/g.\n\n";
  for (const auto& o : options()) {
    std::string flag = std::string("  --") + o.name;
    if (o.arg != nullptr) flag += std::string(" ") + o.arg;
    os << flag;
    if (flag.size() < 34) os << std::string(34 - flag.size(), ' ');
    else os << "\n" << std::string(34, ' ');
    os << o.help << "\n";
  }
  os << "  --config FILE                   load options from FILE\n"
     << "  --help, --version\n\n"
     << "Signals: SIGTERM/SIGINT drain gracefully (twice = stop now), SIGQUIT stops now,\n"
     << "         SIGHUP reopens the access log. Supports systemd socket activation (LISTEN_FDS).\n";
  return os.str();
}

}  // namespace hws
