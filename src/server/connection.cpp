#include "server/connection.h"

#include <sys/sendfile.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>

#include "http/status.h"
#include "server/worker.h"
#include "util/clock.h"

namespace hws {
namespace {

constexpr int kMaxReadsPerEvent = 16;                 // then yield to other connections
constexpr size_t kReadChunk = 16 * 1024;              // growth step for a connection's own buffer
constexpr size_t kOutputHighWatermark = 256 * 1024;   // stop parsing pipelined requests beyond this
constexpr uint64_t kSendfileBudget = 4ull << 20;      // bytes per wakeup before yielding
constexpr uint64_t kSendfileChunk = 1ull << 20;
constexpr uint32_t kLingerMaxBytes = 1u << 20;
constexpr std::string_view k100Continue = "HTTP/1.1 100 Continue\r\n\r\n";

}  // namespace

Connection::Connection(Worker& worker, int fd, const sockaddr_in6& peer) noexcept : w_(worker), fd_(fd), peer_(peer) {}

Connection::~Connection() {
  if (fd_ >= 0) ::close(fd_);
}

bool Connection::start() {
  if (!w_.loop().add(fd_, EPOLLIN | EPOLLOUT | EPOLLRDHUP | EPOLLET, this)) return false;
  set_deadline(Deadline::Header, w_.now_ms() + w_.cfg().header_timeout_ms);
  return true;
}

Buffer& Connection::sink() noexcept { return output_pending() ? out_ : w_.out_scratch(); }

void Connection::on_io(uint32_t events) {
  if (phase_ == Phase::Closed) return;  // stale event for a connection closed earlier in this batch
  if (events & EPOLLERR) {
    close_now();
    return;
  }
  if ((events & EPOLLOUT) && output_pending()) {
    if (!write_output()) return;
    if (output_pending()) {
      set_deadline(Deadline::Write, w_.now_ms() + w_.cfg().write_timeout_ms);  // progress was made
    } else {
      output_drained();
      if (closed()) return;
    }
  }
  if (events & (EPOLLIN | EPOLLRDHUP | EPOLLHUP)) {
    if (phase_ == Phase::Linger) {
      linger_read();
      return;
    }
    if (output_pending()) {
      if (events & EPOLLHUP) {
        close_now();  // peer is gone; nothing left to deliver to
        return;
      }
      more_to_read_ = true;  // backpressure: read again once the client drains our output
      return;
    }
    read_input();
  }
}

void Connection::read_input() {
  char* const scratch = w_.in_scratch();
  const size_t scratch_cap = w_.in_scratch_size();
  more_to_read_ = false;
  for (int iter = 0; iter < kMaxReadsPerEvent; ++iter) {
    const bool direct = in_.empty();
    char* dst = scratch;
    size_t cap = scratch_cap;
    if (!direct) {
      in_.ensure_writable(kReadChunk);
      dst = in_.write_ptr();
      cap = in_.writable();
    }
    const ssize_t n = ::read(fd_, dst, cap);
    if (n > 0) {
      const auto got = static_cast<size_t>(n);
      w_.metrics().bytes_in.add(got);
      if (unrecorded_ == 0) batch_start_ns_ = monotonic_ns();
      if (direct) {
        const size_t used = process(dst, got);
        if (closed()) return;
        if (close_after_output_) {
          if (used < got) linger_on_close_ = true;
        } else if (used < got) {
          in_.append(dst + used, got - used);
        }
      } else {
        in_.commit(got);
        const size_t used = process(in_.data(), in_.size());
        if (closed()) return;
        if (close_after_output_) {
          if (used < in_.size()) linger_on_close_ = true;
          in_.clear();
        } else {
          in_.consume(used);
        }
      }
      more_to_read_ = got == cap;
      if (!more_to_read_ || close_after_output_ || stalled_) break;
      continue;
    }
    if (n == 0) {
      peer_eof_ = true;
      more_to_read_ = false;
      break;
    }
    if (errno == EINTR) continue;
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      more_to_read_ = false;
      break;
    }
    close_now();  // ECONNRESET and friends
    return;
  }
  if (more_to_read_ && !close_after_output_ && !stalled_) w_.schedule_ready(this);  // fairness
  after_io();
}

size_t Connection::process(const char* data, size_t len) {
  stalled_ = false;
  const Config& cfg = w_.cfg();
  size_t pos = 0;
  while (pos < len && !close_after_output_) {
    if (file_.active() || sink().size() >= kOutputHighWatermark) {
      stalled_ = true;  // finish what we owe the client before parsing more
      break;
    }
    if (phase_ == Phase::Head) {
      http::Request& req = w_.request();
      const http::ParseResult r = http::parse_request_head({data + pos, len - pos}, req, w_.limits(), scan_from_);
      if (r.status == http::ParseStatus::Incomplete) {
        scan_from_ = len - pos;
        break;
      }
      scan_from_ = 0;
      if (r.status == http::ParseStatus::Error) {
        w_.metrics().parse_errors.add();
        reply_error(r.error_status, r.error_reason);
        return pos;
      }
      const size_t head_begin = pos;
      pos += r.consumed;
      ++requests_;
      if (req.body_kind == http::BodyKind::Length && req.content_length > cfg.max_body_bytes) {
        reply_error(413, "request body exceeds max-body-bytes");
        return pos;
      }
      const size_t avail = len - pos;
      if (req.body_kind == http::BodyKind::None) {
        dispatch(req);
        continue;
      }
      if (req.body_kind == http::BodyKind::Length && avail >= req.content_length) {
        req.body = {data + pos, static_cast<size_t>(req.content_length)};
        pos += static_cast<size_t>(req.content_length);
        dispatch(req);
        continue;
      }
      if (!pending_) pending_ = std::make_unique<PendingBody>();
      PendingBody& pb = *pending_;
      pb.head.clear();
      pb.body.clear();
      pb.chunked.reset();
      pb.kind = req.body_kind;
      if (req.body_kind == http::BodyKind::Chunked) {
        size_t used = 0;
        const auto st = pb.chunked.feed(data + pos, avail, pb.body, cfg.max_body_bytes, used);
        if (st == http::ChunkedDecoder::Status::Error) {
          reply_error(pb.chunked.error_status(), "malformed chunked body");
          return pos;
        }
        if (st == http::ChunkedDecoder::Status::Done) {
          pos += used;
          req.body = pb.body.view();
          dispatch(req);
          pb.body.clear();
          continue;
        }
        pos += used;
      } else {
        pb.body.append(data + pos, avail);
        pb.remaining = req.content_length - avail;
        pos = len;
      }
      // The body continues in later reads: keep the head so its views can be rebuilt.
      pb.head.append(data + head_begin, r.consumed);
      if (req.expect_continue && !req.is_http10()) sink().append(k100Continue);
      phase_ = Phase::Body;
      set_deadline(Deadline::Body, w_.now_ms() + cfg.body_timeout_ms);
      break;
    }

    // Phase::Body
    PendingBody& pb = *pending_;
    if (pb.kind == http::BodyKind::Length) {
      const auto take = static_cast<size_t>(std::min<uint64_t>(pb.remaining, len - pos));
      pb.body.append(data + pos, take);
      pos += take;
      pb.remaining -= take;
      if (pb.remaining > 0) break;
    } else {
      size_t used = 0;
      const auto st = pb.chunked.feed(data + pos, len - pos, pb.body, cfg.max_body_bytes, used);
      pos += used;
      if (st == http::ChunkedDecoder::Status::Error) {
        reply_error(pb.chunked.error_status(), "malformed chunked body");
        return pos;
      }
      if (st == http::ChunkedDecoder::Status::NeedMore) break;
    }
    http::Request& req = w_.request();
    const http::ParseResult r = http::parse_request_head(pb.head.view(), req, w_.limits());
    phase_ = Phase::Head;
    if (r.status != http::ParseStatus::Complete) {  // cannot happen: the head parsed before
      reply_error(500, "internal error");
      return pos;
    }
    req.body = pb.body.view();
    dispatch(req);
    if (pb.body.capacity() > (64u << 10)) {
      pending_.reset();  // do not keep large upload buffers on a keep-alive connection
    } else {
      pb.head.clear();
      pb.body.clear();
    }
  }
  return pos;
}

void Connection::dispatch(http::Request& req) {
  deadline_kind_ = Deadline::None;  // the next request gets a fresh deadline
  http::Response& resp = w_.response();
  resp.reset();
  w_.handle(req, resp);
  const uint64_t max_requests = w_.cfg().max_requests_per_connection;
  const bool keep = req.keep_alive && !resp.close && !w_.draining() && !peer_eof_ &&
                    (max_requests == 0 || requests_ < max_requests);
  if (!keep) close_after_output_ = true;
  serialize(&req, resp, keep);
  ++unrecorded_;
  w_.metrics().responses[std::min<size_t>(resp.status / 100, 5)].add();
  if (w_.access_log().enabled()) {
    w_.access_log().record(peer(), w_.date().log_time(), &req, resp.status, resp.content_length(),
                           (monotonic_ns() - batch_start_ns_) / 1000);
  }
}

void Connection::reply_error(uint16_t status, const char* reason) {
  http::Response& resp = w_.response();
  resp.reset();
  std::string body(http::reason_phrase(status));
  body.append(": ").append(reason).push_back('\n');
  resp.text(status, body);
  close_after_output_ = true;
  linger_on_close_ = true;  // the client may still be sending the rest of a bad request
  serialize(nullptr, resp, false);
  w_.metrics().responses[std::min<size_t>(status / 100, 5)].add();
  if (w_.access_log().enabled()) {
    w_.access_log().record(peer(), w_.date().log_time(), nullptr, status, resp.content_length(), 0);
  }
}

void Connection::serialize(const http::Request* req, const http::Response& resp, bool keep_alive) {
  Buffer& o = sink();
  o.append(http::status_line(resp.status));
  o.append(w_.common_headers());
  o.append(resp.headers());
  const bool has_body = http::status_has_body(resp.status);
  const uint64_t length = resp.content_length();
  if (has_body) {
    o.append("Content-Length: ");
    o.append_u64(length);
    o.append("\r\n");
  }
  if (!keep_alive) {
    o.append("Connection: close\r\n");
  } else if (req != nullptr && req->is_http10()) {
    o.append("Connection: keep-alive\r\n");
  }
  o.append("\r\n");
  const bool head_only = req != nullptr && req->method == http::Method::Head;
  if (!has_body || head_only || length == 0) return;
  if (resp.file().active()) {
    file_ = resp.file();
  } else {
    o.append(resp.body());
  }
}

bool Connection::write_output() {
  Buffer& scratch = w_.out_scratch();
  const int more = file_.active() ? MSG_MORE : 0;
  if (!scratch.empty()) {
    // Invariant: out_ is empty whenever the shared scratch holds this connection's output.
    ssize_t n = ::send(fd_, scratch.data(), scratch.size(), MSG_NOSIGNAL | more);
    if (n < 0) {
      if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
        scratch.clear();
        close_now();
        return false;
      }
      n = 0;
    }
    const auto sent = static_cast<size_t>(n);
    w_.metrics().bytes_out.add(sent);
    if (sent < scratch.size()) out_.append(scratch.data() + sent, scratch.size() - sent);
    scratch.clear();
    if (!out_.empty()) return true;  // socket buffer full: wait for the EPOLLOUT edge
  }
  while (!out_.empty()) {
    const ssize_t n = ::send(fd_, out_.data(), out_.size(), MSG_NOSIGNAL | more);
    if (n > 0) {
      w_.metrics().bytes_out.add(static_cast<uint64_t>(n));
      out_.consume(static_cast<size_t>(n));
      continue;
    }
    if (n < 0 && errno == EINTR) continue;
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return true;
    close_now();
    return false;
  }
  out_.release();
  uint64_t budget = kSendfileBudget;
  while (file_.active()) {
    auto off = static_cast<off_t>(file_.offset);
    const uint64_t want = std::min({file_.length, budget, kSendfileChunk});
    const ssize_t n = ::sendfile(fd_, file_.fd, &off, static_cast<size_t>(want));
    if (n > 0) {
      const auto sent = static_cast<uint64_t>(n);
      w_.metrics().bytes_out.add(sent);
      file_.offset += sent;
      file_.length -= sent;
      budget -= sent;
      if (file_.length == 0) {
        file_ = http::FileBody{};
        break;
      }
      if (budget == 0) {
        w_.schedule_ready(this);  // let other connections run; continue on the next loop turn
        return true;
      }
      continue;
    }
    if (n == 0) {  // file shrank underneath us: Content-Length can no longer be honored
      close_now();
      return false;
    }
    if (errno == EINTR) continue;
    if (errno == EAGAIN || errno == EWOULDBLOCK) return true;
    close_now();
    return false;
  }
  return true;
}

void Connection::after_io() {
  if (!write_output()) return;
  if (output_pending()) {
    refresh_deadline();
    return;
  }
  output_drained();
}

void Connection::output_drained() {
  record_latency();
  // While draining, a connection closes as soon as it has nothing left to send, even if the
  // response it just finished was promised keep-alive before shutdown began.
  if (close_after_output_ || (w_.draining() && phase_ == Phase::Head)) {
    close_gracefully();
    return;
  }
  if (stalled_ && !in_.empty()) {
    w_.schedule_ready(this);  // buffered pipelined requests are waiting
    refresh_deadline();
    return;
  }
  if (peer_eof_) {
    close_now();  // the client is done sending and everything it asked for was answered
    return;
  }
  if (more_to_read_) w_.schedule_ready(this);
  refresh_deadline();
}

void Connection::refresh_deadline() noexcept {
  const Config& cfg = w_.cfg();
  const uint64_t now = w_.now_ms();
  if (phase_ == Phase::Linger || phase_ == Phase::Closed) return;
  if (output_pending()) {
    set_deadline(Deadline::Write, now + cfg.write_timeout_ms);
  } else if (phase_ == Phase::Body) {
    if (deadline_kind_ != Deadline::Body) set_deadline(Deadline::Body, now + cfg.body_timeout_ms);
  } else if (!in_.empty() || requests_ == 0) {
    // Partial head, or a fresh connection that has sent nothing: absolute header deadline.
    if (deadline_kind_ != Deadline::Header) set_deadline(Deadline::Header, now + cfg.header_timeout_ms);
  } else {
    set_deadline(Deadline::Idle, now + cfg.keepalive_timeout_ms);
  }
}

void Connection::on_ready() {
  if (closed() || phase_ == Phase::Linger) return;
  if (output_pending()) {
    if (!write_output()) return;
    if (output_pending()) {
      refresh_deadline();
      return;
    }
    output_drained();
    return;
  }
  if (stalled_ && !in_.empty()) {
    if (unrecorded_ == 0) batch_start_ns_ = monotonic_ns();
    const size_t used = process(in_.data(), in_.size());
    if (closed()) return;
    if (close_after_output_) {
      if (used < in_.size()) linger_on_close_ = true;
      in_.clear();
    } else {
      in_.consume(used);
    }
    if (!stalled_ && more_to_read_ && !close_after_output_) {
      read_input();  // also flushes
      return;
    }
    after_io();
    return;
  }
  if (more_to_read_) read_input();
}

void Connection::on_timer() {
  if (closed() || deadline_kind_ == Deadline::None) return;
  const uint64_t now = w_.now_ms();
  if (now < deadline_ms_) {  // deadline was pushed back since this node was scheduled
    scheduled_ms_ = deadline_ms_;
    w_.timers().schedule(this, deadline_ms_);
    return;
  }
  WorkerMetrics& m = w_.metrics();
  switch (deadline_kind_) {
    case Deadline::Idle:
      m.timeout_idle.add();
      close_now();
      return;
    case Deadline::Header:
      m.timeout_header.add();
      if (in_.empty()) {
        close_now();  // never sent a byte: close silently
        return;
      }
      in_.clear();
      reply_error(408, "request head not received in time");
      after_io();
      return;
    case Deadline::Body:
      m.timeout_body.add();
      reply_error(408, "request body not received in time");
      after_io();
      return;
    case Deadline::Write:
      m.timeout_write.add();
      close_now();
      return;
    case Deadline::Linger:
    case Deadline::None:
      close_now();
      return;
  }
}

void Connection::begin_drain() {
  if (phase_ == Phase::Closed || phase_ == Phase::Linger) return;
  if (idle()) close_now();
  // Otherwise the in-flight response carries "Connection: close" (dispatch checks draining).
}

void Connection::abort() { close_now(); }

void Connection::close_gracefully() {
  if (!linger_on_close_ || peer_eof_) {
    close_now();
    return;
  }
  // RFC 9112 §9.6: closing with unread input makes the kernel send RST, which can destroy the
  // response before the client reads it. Half-close and drain the rest for a short while.
  ::shutdown(fd_, SHUT_WR);
  phase_ = Phase::Linger;
  in_.release();
  pending_.reset();
  set_deadline(Deadline::Linger, w_.now_ms() + w_.cfg().linger_timeout_ms);
  linger_read();
}

void Connection::linger_read() {
  char* const buf = w_.in_scratch();
  const size_t cap = w_.in_scratch_size();
  while (true) {
    const ssize_t n = ::read(fd_, buf, cap);
    if (n > 0) {
      linger_bytes_ += static_cast<uint32_t>(n);
      if (linger_bytes_ > kLingerMaxBytes) break;
      continue;
    }
    if (n < 0 && errno == EINTR) continue;
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
    break;  // EOF or error
  }
  close_now();
}

void Connection::close_now() {
  if (phase_ == Phase::Closed) return;
  phase_ = Phase::Closed;
  w_.timers().cancel(this);
  ::close(fd_);  // also removes the descriptor from epoll
  fd_ = -1;
  in_.release();
  out_.release();
  file_ = http::FileBody{};
  pending_.reset();
  w_.on_closed(this);
}

void Connection::set_deadline(Deadline kind, uint64_t at_ms) noexcept {
  deadline_kind_ = kind;
  deadline_ms_ = at_ms;
  // Lazy rescheduling: pushing a deadline later only updates deadline_ms_; on_timer() moves the
  // node when the old tick arrives. Only an earlier deadline needs the wheel touched.
  if (!timer_linked() || at_ms < scheduled_ms_) {
    scheduled_ms_ = at_ms;
    w_.timers().schedule(this, at_ms);
  }
}

void Connection::record_latency() noexcept {
  if (unrecorded_ == 0) return;
  w_.metrics().latency.observe(monotonic_ns() - batch_start_ns_, unrecorded_);
  unrecorded_ = 0;
}

}  // namespace hws
