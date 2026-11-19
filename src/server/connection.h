#pragma once

#include <netinet/in.h>

#include <cstdint>
#include <memory>

#include "http/chunked.h"
#include "http/parser.h"
#include "http/response.h"
#include "net/event_loop.h"
#include "util/buffer.h"
#include "util/timer_wheel.h"

namespace hws {

class Worker;

// One client connection, owned and driven by a single worker thread.
//
// Memory: input is read into the worker's shared scratch buffer and parsed in place; only bytes
// of an incomplete request are copied into `in_`. Responses are serialized into the worker's
// shared output buffer and written in one send(); only what the kernel refuses is copied into
// `out_`. An idle keep-alive connection therefore owns no heap buffers, which is what makes
// hundreds of thousands of concurrent connections cheap.
//
// The socket is registered once with EPOLLIN|EPOLLOUT|EPOLLRDHUP|EPOLLET and never modified, so
// steady-state request handling makes no epoll_ctl calls at all.
class Connection final : public net::IoHandler, public TimerNode {
 public:
  Connection(Worker& worker, int fd, const sockaddr_in6& peer) noexcept;
  ~Connection() override;
  Connection(const Connection&) = delete;
  Connection& operator=(const Connection&) = delete;

  bool start();                       // register with epoll; false means the caller must abort()
  void on_io(uint32_t events) override;
  void on_timer();                    // the timer wheel fired this connection's node
  void on_ready();                    // deferred work queued with Worker::schedule_ready()
  void begin_drain();                 // graceful shutdown: close now if idle, else after this response
  void abort();                       // close immediately

  bool closed() const noexcept { return phase_ == Phase::Closed; }
  bool idle() const noexcept { return phase_ == Phase::Head && in_.empty() && !output_pending(); }
  const sockaddr* peer() const noexcept { return reinterpret_cast<const sockaddr*>(&peer_); }

  // Worker bookkeeping.
  Connection* prev_live = nullptr;
  Connection* next_live = nullptr;
  bool queued_ready = false;

 private:
  enum class Phase : uint8_t { Head, Body, Linger, Closed };
  enum class Deadline : uint8_t { None, Idle, Header, Body, Write, Linger };

  // Exists only while a request body spans several reads.
  struct PendingBody {
    Buffer head;  // the request head, re-parsed once the body is complete
    Buffer body;
    http::ChunkedDecoder chunked;
    uint64_t remaining = 0;
    http::BodyKind kind = http::BodyKind::None;
  };

  void read_input();
  size_t process(const char* data, size_t len);
  void dispatch(http::Request& req);
  void reply_error(uint16_t status, const char* reason);
  void serialize(const http::Request* req, const http::Response& resp, bool keep_alive);
  bool write_output();  // false: the connection was closed
  void after_io();
  void output_drained();
  void refresh_deadline() noexcept;
  void close_gracefully();
  void linger_read();
  void close_now();
  void set_deadline(Deadline kind, uint64_t at_ms) noexcept;
  void record_latency() noexcept;
  bool output_pending() const noexcept { return !out_.empty() || file_.active(); }
  Buffer& sink() noexcept;

  Worker& w_;
  int fd_;
  Phase phase_ = Phase::Head;
  Deadline deadline_kind_ = Deadline::None;
  bool close_after_output_ = false;
  bool linger_on_close_ = false;  // unread client data may follow: half-close and drain first
  bool peer_eof_ = false;
  bool more_to_read_ = false;     // last read filled its buffer, or a read was deferred
  bool stalled_ = false;          // complete requests are buffered but held back (backpressure)
  uint32_t requests_ = 0;
  uint32_t unrecorded_ = 0;       // responses awaiting a latency sample
  uint32_t linger_bytes_ = 0;
  uint64_t batch_start_ns_ = 0;
  uint64_t deadline_ms_ = 0;
  uint64_t scheduled_ms_ = 0;
  size_t scan_from_ = 0;          // bytes of the pending head already searched for CRLFCRLF
  Buffer in_;
  Buffer out_;
  http::FileBody file_;
  std::unique_ptr<PendingBody> pending_;
  sockaddr_in6 peer_;
};

}  // namespace hws
