#include "util/buffer.h"

#include <cstdlib>
#include <new>
#include <utility>

#include "util/strings.h"

namespace hws {

Buffer::~Buffer() { std::free(data_); }

Buffer::Buffer(Buffer&& other) noexcept
    : data_(std::exchange(other.data_, nullptr)),
      cap_(std::exchange(other.cap_, 0)),
      rd_(std::exchange(other.rd_, 0)),
      wr_(std::exchange(other.wr_, 0)) {}

Buffer& Buffer::operator=(Buffer&& other) noexcept {
  if (this != &other) {
    std::free(data_);
    data_ = std::exchange(other.data_, nullptr);
    cap_ = std::exchange(other.cap_, 0);
    rd_ = std::exchange(other.rd_, 0);
    wr_ = std::exchange(other.wr_, 0);
  }
  return *this;
}

void Buffer::release() noexcept {
  std::free(data_);
  data_ = nullptr;
  cap_ = rd_ = wr_ = 0;
}

void Buffer::ensure_writable(size_t n) {
  if (cap_ - wr_ >= n) return;
  const size_t used = wr_ - rd_;
  // Reclaim consumed prefix first when that alone makes room.
  if (rd_ > 0 && cap_ - used >= n) {
    std::memmove(data_, data_ + rd_, used);
    rd_ = 0;
    wr_ = used;
    return;
  }
  size_t new_cap = cap_ < 1024 ? 1024 : cap_ * 2;
  while (new_cap - used < n) new_cap *= 2;
  if (rd_ > 0) {
    std::memmove(data_, data_ + rd_, used);
    rd_ = 0;
    wr_ = used;
  }
  void* p = std::realloc(data_, new_cap);
  if (p == nullptr) throw std::bad_alloc();
  data_ = static_cast<char*>(p);
  cap_ = new_cap;
}

void Buffer::append_u64(uint64_t v) {
  ensure_writable(20);
  wr_ += format_u64(data_ + wr_, v);
}

}  // namespace hws
