#include "server/static_files.h"

#include <cerrno>
#include <cstdio>

#include "http/date.h"
#include "http/range.h"
#include "http/uri.h"
#include "server/metrics.h"

namespace hws {
namespace {

void error_from_errno(int e, http::Response& resp) {
  switch (e) {
    case ENOENT:
    case ENOTDIR:
    case EXDEV:    // resolution escaped root
    case ELOOP:    // symlink refused (follow-symlinks off) or loop
    case ENAMETOOLONG:
      resp.text(404, "Not Found\n");
      break;
    case EACCES:
    case EPERM:
      resp.text(403, "Forbidden\n");
      break;
    default:
      resp.text(500, "Internal Server Error\n");
      break;
  }
}

}  // namespace

void StaticFiles::serve(const http::Request& req, std::string_view path, FileCache& cache, uint64_t now_ms,
                        http::Response& resp, WorkerMetrics& metrics) {
  if (req.method != http::Method::Get && req.method != http::Method::Head) {
    resp.text(405, "Method Not Allowed\n");
    resp.add_header("Allow", "GET, HEAD");
    return;
  }
  if (!opts_.serve_dotfiles && http::has_dot_segment(path)) {
    resp.text(404, "Not Found\n");
    return;
  }
  const bool want_dir = path.back() == '/';
  std::string_view rel = path.substr(1);
  if (want_dir && !rel.empty()) rel.remove_suffix(1);

  bool hit = false;
  std::shared_ptr<const CachedFile> f = cache.get(rel, now_ms, hit);
  (hit ? metrics.file_cache_hits : metrics.file_cache_misses).add();
  if (f->error != 0) {
    error_from_errno(f->error, resp);
    return;
  }
  if (f->is_dir) {
    if (!want_dir) {
      scratch_.assign(path);
      scratch_.push_back('/');
      if (!req.query.empty()) {
        scratch_.push_back('?');
        scratch_.append(req.query);
      }
      resp.text(301, "Moved Permanently\n");
      resp.add_header("Location", scratch_);
      return;
    }
    scratch_.assign(rel);
    if (!scratch_.empty()) scratch_.push_back('/');
    scratch_.append(opts_.index);
    f = cache.get(scratch_, now_ms, hit);
    (hit ? metrics.file_cache_hits : metrics.file_cache_misses).add();
    if (f->error == ENOENT || f->is_dir) {
      resp.text(403, "Forbidden\n");  // no autoindex
      return;
    }
    if (f->error != 0) {
      error_from_errno(f->error, resp);
      return;
    }
  } else if (want_dir) {
    resp.text(404, "Not Found\n");
    return;
  }
  serve_file(req, f, resp);
}

void StaticFiles::common_headers(const CachedFile& f, http::Response& resp, bool with_type) {
  if (with_type) resp.add_header("Content-Type", f.content_type);
  resp.add_header("Last-Modified", f.last_modified);
  resp.add_header("ETag", f.etag);
  if (!opts_.cache_control.empty()) resp.add_header("Cache-Control", opts_.cache_control);
}

void StaticFiles::serve_file(const http::Request& req, const std::shared_ptr<const CachedFile>& f,
                             http::Response& resp) {
  // Conditional GET (RFC 9110 §13.2.2): If-None-Match takes precedence over If-Modified-Since.
  bool not_modified = false;
  if (!req.if_none_match.empty()) {
    not_modified = http::if_none_match_matches(req.if_none_match, f->etag);
  } else if (!req.if_modified_since.empty()) {
    time_t since = 0;
    not_modified = http::parse_http_date(req.if_modified_since, since) && f->mtime.tv_sec <= since;
  }
  if (not_modified) {
    resp.status = 304;
    common_headers(*f, resp, false);
    return;
  }

  uint64_t start = 0;
  uint64_t length = f->size;
  resp.status = 200;
  if (!req.range.empty()) {
    bool honor = true;
    if (!req.if_range.empty()) {
      if (req.if_range.front() == '"' || req.if_range.substr(0, 2) == "W/") {
        honor = http::strong_etag_equal(req.if_range, f->etag);
      } else {
        time_t t = 0;
        honor = http::parse_http_date(req.if_range, t) && t == f->mtime.tv_sec;
      }
    }
    if (honor) {
      uint64_t s = 0;
      uint64_t n = 0;
      switch (http::parse_range(req.range, f->size, s, n)) {
        case http::RangeResult::Satisfiable: {
          resp.status = 206;
          start = s;
          length = n;
          char cr[80];
          const int k = std::snprintf(cr, sizeof(cr), "bytes %llu-%llu/%llu", static_cast<unsigned long long>(s),
                                      static_cast<unsigned long long>(s + n - 1),
                                      static_cast<unsigned long long>(f->size));
          resp.add_header("Content-Range", std::string_view(cr, k > 0 ? static_cast<size_t>(k) : 0));
          break;
        }
        case http::RangeResult::Unsatisfiable: {
          resp.text(416, "Range Not Satisfiable\n");
          char cr[48];
          const int k = std::snprintf(cr, sizeof(cr), "bytes */%llu", static_cast<unsigned long long>(f->size));
          resp.add_header("Content-Range", std::string_view(cr, k > 0 ? static_cast<size_t>(k) : 0));
          return;
        }
        case http::RangeResult::Ignore:
          break;
      }
    }
  }

  common_headers(*f, resp, true);
  resp.add_header("Accept-Ranges", "bytes");
  if (f->inlined) {
    resp.set_body_view(std::string_view(f->content).substr(static_cast<size_t>(start), static_cast<size_t>(length)), f);
  } else {
    resp.set_file(http::FileBody{f, f->fd.get(), start, length});
  }
}

}  // namespace hws
