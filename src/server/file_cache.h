#pragma once

#include <sys/stat.h>

#include <cstdint>
#include <list>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>

#include "util/fd.h"

namespace hws {

struct CachedFile {
  int error = 0;        // errno of a failed lookup (negative entry); 0 on success
  bool is_dir = false;
  bool inlined = false;  // body held in `content`, descriptor closed
  UniqueFd fd;           // open for regular files that are not inlined
  uint64_t size = 0;
  dev_t dev = 0;
  ino_t ino = 0;
  timespec mtime{};
  std::string etag;           // "\"<mtime hex>-<size hex>\"" like nginx
  std::string last_modified;  // IMF-fixdate
  std::string_view content_type;
  std::string content;
  uint64_t validated_ms = 0;
};

struct FileCacheOptions {
  size_t max_entries = 1024;
  uint64_t ttl_ms = 2000;
  size_t inline_max = 16 * 1024;
  size_t inline_budget = 32u << 20;
  bool follow_symlinks = true;
};

// Per-worker LRU cache of open files and their metadata. Lookups resolve relative to the document
// root with openat2(RESOLVE_BENEATH | RESOLVE_NO_MAGICLINKS), so neither ".." nor symlinks can
// reach outside root. Entries are revalidated with fstatat() once their TTL passes; a changed
// inode, size or mtime reloads the entry (handles atomic-rename deploys).
class FileCache {
 public:
  FileCache(int root_fd, const FileCacheOptions& opts);
  FileCache(const FileCache&) = delete;
  FileCache& operator=(const FileCache&) = delete;

  // `rel` is a normalized path without the leading '/' ("" is the root). Never returns null.
  std::shared_ptr<const CachedFile> get(std::string_view rel, uint64_t now_ms, bool& hit);
  size_t size() const noexcept { return index_.size(); }
  size_t inline_bytes() const noexcept { return inline_bytes_; }

 private:
  using Node = std::pair<std::string, std::shared_ptr<CachedFile>>;

  std::shared_ptr<CachedFile> load(const char* rel, uint64_t now_ms);
  bool revalidate(CachedFile& f, const char* rel, uint64_t now_ms) const;
  void erase(std::list<Node>::iterator it);

  int root_fd_;
  FileCacheOptions opts_;
  size_t inline_bytes_ = 0;
  std::string path_;  // NUL-terminated scratch copy of the lookup key
  std::list<Node> lru_;
  std::unordered_map<std::string_view, std::list<Node>::iterator> index_;
};

}  // namespace hws
