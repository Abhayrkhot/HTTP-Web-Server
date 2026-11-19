#include "http/chunked.h"

#include <algorithm>

#include "http/chars.h"

namespace hws::http {

ChunkedDecoder::Status ChunkedDecoder::feed(const char* p, size_t n, Buffer& out, uint64_t max_body,
                                            size_t& consumed) {
  size_t i = 0;
  while (i < n) {
    const char c = p[i];
    switch (state_) {
      case State::Size:
        if (is_hex(c)) {
          if (++size_digits_ > 15) return error(413);  // > 2^60 bytes
          remaining_ = remaining_ * 16 + static_cast<uint64_t>(hex_value(c));
          ++i;
        } else if (size_digits_ == 0) {
          return error(400);
        } else if (c == ';' || c == ' ' || c == '\t') {
          state_ = State::Ext;  // chunk-ext (and BWS before it); content is ignored
          ++i;
        } else if (c == '\r') {
          state_ = State::SizeLF;
          ++i;
        } else {
          return error(400);
        }
        break;
      case State::Ext:
        if (c == '\r') {
          state_ = State::SizeLF;
        } else if (c == '\n' || c == 0x7f || (static_cast<unsigned char>(c) < 0x20 && c != '\t')) {
          return error(400);
        } else if (++meta_bytes_ > kMaxMetaBytes) {
          return error(400);
        }
        ++i;
        break;
      case State::SizeLF:
        if (c != '\n') return error(400);
        ++i;
        size_digits_ = 0;
        if (remaining_ == 0) {
          state_ = State::TrailerStart;
        } else {
          if (remaining_ > max_body || total_ + remaining_ > max_body) return error(413);
          total_ += remaining_;
          state_ = State::Data;
        }
        break;
      case State::Data: {
        const size_t take = static_cast<size_t>(std::min<uint64_t>(remaining_, n - i));
        out.append(p + i, take);
        i += take;
        remaining_ -= take;
        if (remaining_ == 0) state_ = State::DataCR;
        break;
      }
      case State::DataCR:
        if (c != '\r') return error(400);
        state_ = State::DataLF;
        ++i;
        break;
      case State::DataLF:
        if (c != '\n') return error(400);
        state_ = State::Size;
        ++i;
        break;
      case State::TrailerStart:
        if (c == '\r') {
          state_ = State::FinalLF;
          ++i;
        } else {
          state_ = State::TrailerLine;  // re-examine this byte as trailer content
        }
        break;
      case State::TrailerLine:
        if (c == '\r') {
          state_ = State::TrailerLF;
        } else if (c == '\n' || c == 0x7f || (static_cast<unsigned char>(c) < 0x20 && c != '\t')) {
          return error(400);
        } else if (++meta_bytes_ > kMaxMetaBytes) {
          return error(400);
        }
        ++i;
        break;
      case State::TrailerLF:
        if (c != '\n') return error(400);
        state_ = State::TrailerStart;
        ++i;
        break;
      case State::FinalLF:
        if (c != '\n') return error(400);
        consumed = i + 1;
        return Status::Done;
    }
  }
  consumed = n;
  return Status::NeedMore;
}

}  // namespace hws::http
