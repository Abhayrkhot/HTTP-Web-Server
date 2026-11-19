#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace hws {

struct Config {
  // Listening
  std::vector<std::string> listen{"0.0.0.0:8080"};
  unsigned workers = 0;           // 0: one per online CPU
  bool reuseport = true;          // per-worker SO_REUSEPORT sockets; off: one shared socket + EPOLLEXCLUSIVE
  int backlog = 65535;
  int defer_accept_s = 0;
  bool pin_workers = false;       // pin worker i to the i-th CPU of the process affinity mask
  uint64_t max_connections = 0;   // 0: derive from RLIMIT_NOFILE

  // Static files
  std::string root;               // empty disables static file serving
  std::string index = "index.html";
  bool serve_dotfiles = false;
  bool follow_symlinks = true;    // symlinks are always confined to root (openat2 RESOLVE_BENEATH)
  std::string cache_control;
  uint64_t file_cache_entries = 1024;          // per worker
  uint64_t file_cache_ttl_ms = 2000;
  uint64_t inline_file_max = 16 * 1024;        // files up to this size are served from memory
  uint64_t inline_cache_bytes = 32ull << 20;   // per worker budget for in-memory file bodies

  // Timeouts
  uint64_t keepalive_timeout_ms = 60000;
  uint64_t header_timeout_ms = 10000;
  uint64_t body_timeout_ms = 30000;
  uint64_t write_timeout_ms = 30000;
  uint64_t linger_timeout_ms = 2000;
  uint64_t drain_timeout_ms = 30000;

  // Limits
  uint64_t max_header_bytes = 16 * 1024;
  uint64_t max_headers = 100;
  uint64_t max_uri_bytes = 8 * 1024;
  uint64_t max_body_bytes = 1ull << 20;
  uint64_t max_requests_per_connection = 10000;  // 0: unlimited

  // Logging and built-in endpoints
  std::string access_log = "off";  // off | stdout | /path/to/file
  std::string log_level = "info";
  std::string server_name = "hws";
  std::string health_path = "/healthz";
  std::string metrics_path = "/metrics";
  std::string hello_path;  // plaintext "Hello, World!" endpoint (benchmarks); empty disables
  std::string echo_path;   // POST echo endpoint (tests); empty disables
};

enum class ArgsResult { Run, Exit, Error };

// Parses argv into cfg. "--config FILE" is applied first so command-line flags override it.
ArgsResult parse_args(int argc, char** argv, Config& cfg, std::string& err);
bool load_config_file(const std::string& path, Config& cfg, std::string& err);
bool validate_config(Config& cfg, std::string& err);
std::string usage();

bool parse_duration_ms(std::string_view text, uint64_t& out) noexcept;  // "250ms", "10s", "2m", "1h", "500"
bool parse_size(std::string_view text, uint64_t& out) noexcept;         // "512", "16k", "1m", "2g"

}  // namespace hws
