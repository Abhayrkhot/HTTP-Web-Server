// Differential fuzzing of the chunked decoder: decoding the input in one call must give exactly
// the same outcome, payload and consumed count as decoding it split at arbitrary points.
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>

#include "http/chunked.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  if (size < 2) return 0;
  const uint64_t max_body = (static_cast<uint64_t>(data[0]) << 8) | 64;  // vary the limit
  const uint8_t step_seed = data[1];
  const char* p = reinterpret_cast<const char*>(data + 2);
  const size_t n = size - 2;

  using hws::http::ChunkedDecoder;
  ChunkedDecoder one;
  hws::Buffer out1;
  size_t used1 = 0;
  const ChunkedDecoder::Status s1 = one.feed(p, n, out1, max_body, used1);

  ChunkedDecoder split;
  hws::Buffer out2;
  size_t pos = 0;
  ChunkedDecoder::Status s2 = ChunkedDecoder::Status::NeedMore;
  size_t step = 1 + step_seed % 13;
  while (pos < n) {
    const size_t len = step < n - pos ? step : n - pos;
    size_t used = 0;
    s2 = split.feed(p + pos, len, out2, max_body, used);
    if (used > len) std::abort();
    pos += used;
    if (s2 != ChunkedDecoder::Status::NeedMore) break;
    step = 1 + (step * 7 + 3) % 17;
  }

  if (s1 != s2) std::abort();
  if (s1 == ChunkedDecoder::Status::Error && one.error_status() != split.error_status()) std::abort();
  if (s1 == ChunkedDecoder::Status::Done) {
    if (used1 != pos) std::abort();
    if (out1.view() != out2.view()) std::abort();
  }
  if (out1.size() > max_body) std::abort();
  return 0;
}
