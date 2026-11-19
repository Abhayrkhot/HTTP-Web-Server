#pragma once

#include <cstddef>
#include <cstdint>

#include "util/buffer.h"

namespace hws::http {

// Incremental decoder for `Transfer-Encoding: chunked` request bodies (RFC 9112 §7.1).
// Strict CRLF framing, bounded chunk extensions and trailers, overflow-safe sizes.
class ChunkedDecoder {
 public:
  enum class Status : uint8_t { NeedMore, Done, Error };

  void reset() noexcept { *this = ChunkedDecoder{}; }

  // Consumes bytes from [p, p+n), appending decoded payload to `out`. `consumed` is how many input
  // bytes were used: all of them for NeedMore, up to the final CRLF for Done.
  Status feed(const char* p, size_t n, Buffer& out, uint64_t max_body, size_t& consumed);

  // 400 for malformed framing, 413 when the decoded body would exceed max_body.
  uint16_t error_status() const noexcept { return error_; }
  uint64_t decoded_bytes() const noexcept { return total_; }

  static constexpr uint32_t kMaxMetaBytes = 8192;  // chunk-ext + trailer bytes per body

 private:
  enum class State : uint8_t { Size, Ext, SizeLF, Data, DataCR, DataLF, TrailerStart, TrailerLine, TrailerLF, FinalLF };

  Status error(uint16_t status) noexcept {
    error_ = status;
    return Status::Error;
  }

  State state_ = State::Size;
  uint8_t size_digits_ = 0;
  uint16_t error_ = 400;
  uint32_t meta_bytes_ = 0;
  uint64_t remaining_ = 0;
  uint64_t total_ = 0;
};

}  // namespace hws::http
