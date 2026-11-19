#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

namespace hws {

// Growable contiguous byte buffer with a read cursor. Memory is only allocated on first write,
// so idle connections that own empty Buffers cost 32 bytes each.
class Buffer {
 public:
  Buffer() noexcept = default;
  ~Buffer();
  Buffer(const Buffer&) = delete;
  Buffer& operator=(const Buffer&) = delete;
  Buffer(Buffer&& other) noexcept;
  Buffer& operator=(Buffer&& other) noexcept;

  size_t size() const noexcept { return wr_ - rd_; }
  bool empty() const noexcept { return wr_ == rd_; }
  size_t capacity() const noexcept { return cap_; }
  const char* data() const noexcept { return data_ + rd_; }
  std::string_view view() const noexcept { return {data_ + rd_, wr_ - rd_}; }

  // Writable tail. Call ensure_writable() first, then commit() what was written.
  char* write_ptr() noexcept { return data_ + wr_; }
  size_t writable() const noexcept { return cap_ - wr_; }
  void ensure_writable(size_t n);
  void commit(size_t n) noexcept { wr_ += n; }

  void consume(size_t n) noexcept {
    rd_ += n;
    if (rd_ >= wr_) rd_ = wr_ = 0;
  }
  void clear() noexcept { rd_ = wr_ = 0; }
  // Frees the backing allocation.
  void release() noexcept;

  void append(const void* p, size_t n) {
    if (n == 0) return;
    ensure_writable(n);
    std::memcpy(data_ + wr_, p, n);
    wr_ += n;
  }
  void append(std::string_view s) { append(s.data(), s.size()); }
  void append_u64(uint64_t v);

 private:
  char* data_ = nullptr;
  size_t cap_ = 0;
  size_t rd_ = 0;
  size_t wr_ = 0;
};

}  // namespace hws
