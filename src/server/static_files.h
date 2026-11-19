#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include "http/parser.h"
#include "http/response.h"
#include "server/file_cache.h"

namespace hws {

struct WorkerMetrics;

struct StaticOptions {
  std::string index = "index.html";
  bool serve_dotfiles = false;
  std::string cache_control;
};

// GET/HEAD handler for files under the document root: conditional requests (ETag, Last-Modified),
// single byte ranges, directory index and trailing-slash redirects.
class StaticFiles {
 public:
  explicit StaticFiles(StaticOptions opts) : opts_(std::move(opts)) {}

  // `path` is the normalized request path (leading '/').
  void serve(const http::Request& req, std::string_view path, FileCache& cache, uint64_t now_ms,
             http::Response& resp, WorkerMetrics& metrics);

 private:
  void serve_file(const http::Request& req, const std::shared_ptr<const CachedFile>& f, http::Response& resp);
  void common_headers(const CachedFile& f, http::Response& resp, bool with_type);

  StaticOptions opts_;
  std::string scratch_;
};

}  // namespace hws
