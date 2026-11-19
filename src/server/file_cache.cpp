#include "server/file_cache.h"

#include <fcntl.h>
#include <linux/openat2.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstdio>

#include "http/date.h"
#include "http/mime.h"
#include "util/log.h"

namespace hws {
namespace {

std::atomic<bool> g_no_openat2{false};

int open_beneath(int dirfd, const char* rel, bool follow_symlinks) noexcept {
  open_how how{};
  how.flags = O_RDONLY | O_CLOEXEC | O_NOCTTY | O_NONBLOCK;  // O_NONBLOCK: never hang on a FIFO
  how.resolve = RESOLVE_BENEATH | RESOLVE_NO_MAGICLINKS | (follow_symlinks ? 0 : RESOLVE_NO_SYMLINKS);
  if (!g_no_openat2.load(std::memory_order_relaxed)) {
    const long fd = ::syscall(SYS_openat2, dirfd, rel, &how, sizeof(how));
    if (fd >= 0 || errno != ENOSYS) return static_cast<int>(fd);
    if (!g_no_openat2.exchange(true)) {
      LOG_WARN("openat2(2) unavailable (kernel < 5.6): symlinks inside root are not confined");
    }
  }
  return ::openat(dirfd, rel, static_cast<int>(how.flags) | (follow_symlinks ? 0 : O_NOFOLLOW));
}

bool same_file(const CachedFile& f, const struct stat& st) noexcept {
  return f.dev == st.st_dev && f.ino == st.st_ino && f.size == static_cast<uint64_t>(st.st_size) &&
         f.mtime.tv_sec == st.st_mtim.tv_sec && f.mtime.tv_nsec == st.st_mtim.tv_nsec;
}

}  // namespace

FileCache::FileCache(int root_fd, const FileCacheOptions& opts) : root_fd_(root_fd), opts_(opts) {
  if (opts_.max_entries == 0) opts_.max_entries = 1;
  index_.reserve(opts_.max_entries);
}

std::shared_ptr<const CachedFile> FileCache::get(std::string_view rel, uint64_t now_ms, bool& hit) {
  path_.assign(rel.empty() ? std::string_view(".") : rel);
  if (auto it = index_.find(rel); it != index_.end()) {
    CachedFile& f = *it->second->second;
    if (now_ms - f.validated_ms < opts_.ttl_ms || revalidate(f, path_.c_str(), now_ms)) {
      lru_.splice(lru_.begin(), lru_, it->second);
      hit = true;
      return it->second->second;
    }
    erase(it->second);
  }
  hit = false;
  std::shared_ptr<CachedFile> f = load(path_.c_str(), now_ms);
  lru_.emplace_front(std::string(rel), f);
  index_.emplace(lru_.front().first, lru_.begin());
  if (f->inlined) inline_bytes_ += f->content.size();
  while (index_.size() > opts_.max_entries) erase(std::prev(lru_.end()));
  return f;
}

void FileCache::erase(std::list<Node>::iterator it) {
  if (it->second->inlined) inline_bytes_ -= it->second->content.size();
  index_.erase(std::string_view(it->first));
  lru_.erase(it);  // in-flight sendfile transfers keep their own shared_ptr
}

bool FileCache::revalidate(CachedFile& f, const char* rel, uint64_t now_ms) const {
  struct stat st {};
  if (::fstatat(root_fd_, rel, &st, 0) != 0) {
    const int e = errno;
    if (f.error != 0 && (e == ENOENT || e == ENOTDIR) && (f.error == ENOENT || f.error == ENOTDIR)) {
      f.validated_ms = now_ms;  // still missing
      return true;
    }
    return false;
  }
  if (f.error != 0 || !same_file(f, st)) return false;
  f.validated_ms = now_ms;
  return true;
}

std::shared_ptr<CachedFile> FileCache::load(const char* rel, uint64_t now_ms) {
  auto f = std::make_shared<CachedFile>();
  f->validated_ms = now_ms;
  UniqueFd fd{open_beneath(root_fd_, rel, opts_.follow_symlinks)};
  if (!fd) {
    f->error = errno;
    return f;
  }
  struct stat st {};
  if (::fstat(fd.get(), &st) != 0) {
    f->error = errno;
    return f;
  }
  f->dev = st.st_dev;
  f->ino = st.st_ino;
  f->mtime = st.st_mtim;
  if (S_ISDIR(st.st_mode)) {
    f->is_dir = true;
    return f;
  }
  if (!S_ISREG(st.st_mode)) {
    f->error = EACCES;  // devices, FIFOs, sockets
    return f;
  }
  f->size = static_cast<uint64_t>(st.st_size);
  char etag[48];
  const int n = std::snprintf(etag, sizeof(etag), "\"%llx-%llx\"", static_cast<unsigned long long>(st.st_mtim.tv_sec),
                              static_cast<unsigned long long>(st.st_size));
  f->etag.assign(etag, n > 0 ? static_cast<size_t>(n) : 0);
  char date[http::kHttpDateLen];
  http::format_http_date(st.st_mtim.tv_sec, date);
  f->last_modified.assign(date, http::kHttpDateLen);
  f->content_type = http::mime_type_for(rel);

  if (f->size <= opts_.inline_max && inline_bytes_ + f->size <= opts_.inline_budget) {
    f->content.resize(f->size);
    size_t got = 0;
    while (got < f->size) {
      const ssize_t r = ::pread(fd.get(), f->content.data() + got, f->size - got, static_cast<off_t>(got));
      if (r > 0) {
        got += static_cast<size_t>(r);
      } else if (r < 0 && errno == EINTR) {
        continue;
      } else {
        break;
      }
    }
    if (got == f->size) {
      f->inlined = true;
      return f;  // fd closes here: inlined files hold no descriptor
    }
    f->content.clear();
    f->content.shrink_to_fit();
  }
  f->fd = std::move(fd);
  return f;
}

}  // namespace hws
