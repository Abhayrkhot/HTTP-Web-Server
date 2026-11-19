# Architecture

hws is a shared-nothing, multi-reactor HTTP/1.1 server for Linux. Each worker thread runs its own
epoll event loop, owns its own listening socket, and keeps every connection it accepts for that
connection's whole life. Nothing on the request path takes a lock or touches memory shared with
another worker.

```
                       kernel SO_REUSEPORT group (hash of the 4-tuple)
            ┌──────────────────────┬───────────────────────┬──────────────────────┐
            ▼                      ▼                       ▼                      ▼
   ┌─ worker 0 ─────────┐  ┌─ worker 1 ─────────┐  ┌─ worker 2 ─────────┐       ...
   │ listen socket      │  │ listen socket      │  │ listen socket      │
   │ epoll (ET)         │  │ epoll (ET)         │  │ epoll (ET)         │
   │ timer wheel        │  │ timer wheel        │  │ timer wheel        │
   │ connections        │  │ connections        │  │ connections        │
   │ 64 KiB in scratch  │  │ ...                │  │ ...                │
   │ out scratch        │  │                    │  │                    │
   │ open-file cache    │  │                    │  │                    │
   │ access-log buffer  │  │                    │  │                    │
   │ metrics (1 writer) │  │                    │  │                    │
   └────────────────────┘  └────────────────────┘  └────────────────────┘
            ▲ eventfd: drain / stop / reopen-logs posted by the main thread
   main thread: config, sockets, sigtimedwait(SIGTERM, SIGINT, SIGHUP, SIGQUIT)
```

## Layers

| Layer | Files | Responsibility |
|---|---|---|
| `util` | `buffer`, `timer_wheel`, `log`, `fd`, `clock`, `strings` | Byte buffers, O(1) timers, logging, RAII descriptors |
| `net` | `event_loop`, `socket` | epoll wrapper with a cross-thread task queue; listener creation |
| `http` | `parser`, `chunked`, `uri`, `range`, `date`, `mime`, `status`, `response`, `router` | Protocol logic with no I/O: every piece is unit-tested and fuzzed in isolation |
| `server` | `connection`, `worker`, `server`, `static_files`, `file_cache`, `metrics`, `access_log`, `config` | I/O state machines, lifecycle, static files, observability |

The `http` layer never sees a file descriptor; `Connection` never parses bytes itself. Handlers
see a `Request` and fill a `Response`; the connection owns framing (Content-Length, Connection,
Date, Server), so a handler cannot produce a malformed response.

## Connection state machine

```
           accept
             │
             ▼
   ┌────► Head ──── head complete, no body ───► dispatch ──┐
   │        │                                              │
   │        └─ head complete, body pending ─► Body ────────┤ body complete
   │                                                       ▼
   │                                       serialize into output
   │                                                       │
   │      keep-alive and output drained                    │
   └───────────────────────────────────────────────────────┤
                                                           │ close required
                                     unread input? ── yes ─► Linger (shutdown(WR), drain ≤2s)
                                           │no                      │
                                           ▼                        ▼
                                         Closed ◄───────────────────┘
```

Every state has a deadline on the timer wheel:

| Deadline | Default | Starts | Behavior on expiry |
|---|---|---|---|
| header | 10s | first byte of a request (or accept) | 408 + close; silent close if nothing was sent. Absolute, so a slowloris client dribbling bytes cannot extend it |
| body | 30s | request head complete | 408 + close |
| write | 30s | output blocked; extended on every byte of progress | close (client stopped reading) |
| idle | 60s | response fully written on a keep-alive connection | close |
| linger | 2s | half-close after an error response | close |

Deadlines are lazy: extending one only updates a field, and the wheel moves the node when the old
tick arrives. Steady-state keep-alive traffic does not touch the wheel at all.

## The hot path

1. **One epoll registration per connection.** Sockets are added once with
   `EPOLLIN | EPOLLOUT | EPOLLRDHUP | EPOLLET` and never modified. Edge-triggered `EPOLLOUT` only
   fires after a write hit `EAGAIN`, so there is no busy loop and no `epoll_ctl(MOD)` to toggle
   write interest. A keep-alive request costs `epoll_wait` + `read` + `send`.
2. **Zero-copy parsing.** `read()` goes into a per-worker 64 KiB scratch buffer, and the parser
   returns `string_view`s into it. Only bytes of an incomplete request are copied into the
   connection's own buffer. Header names are classified by length and compared case-insensitively
   only when the length matches.
3. **Coalesced writes.** Responses for every pipelined request found in one read are serialized
   into a per-worker output buffer and written with a single `send()`. Only bytes the kernel
   refuses are copied into the connection. The `Date` header is formatted once per second per
   worker.
4. **Small files from memory, large files with `sendfile(2)`.** The open-file cache keeps files up
   to `inline-file-max` (16 KiB) in memory, so they cost one `send()`; bigger files are streamed
   from a cached descriptor with `sendfile`, headers sent with `MSG_MORE` so they share the first
   segment.
5. **Single-writer metrics.** Counters are `std::atomic<uint64_t>` written with a relaxed load and
   store by their owning worker only (no `lock`-prefixed instructions). `/metrics` sums them.

## Memory per connection

An idle keep-alive connection owns its `Connection` object (264 bytes) and no heap buffers:
input lives in the worker's scratch buffer unless a request is split across reads, output is
released as soon as the kernel accepts it, and request-body state is allocated only while a body
is in flight. This is what lets a few cores hold hundreds of thousands of connections; see
[BENCHMARKS.md](BENCHMARKS.md#concurrency).

## Fairness and backpressure

* A connection reads at most 16 times per wakeup, then yields; leftover work goes on the worker's
  ready list, processed after the current epoll batch with a zero timeout.
* `sendfile` sends at most 4 MiB per wakeup before yielding, so one fast download cannot starve
  the other connections on that worker.
* When a connection has 256 KiB of unsent output, the server stops parsing that client's
  pipelined requests until it reads. A client that never reads hits the write timeout.
* The accept loop takes at most 64 connections per wakeup.

## Protocol strictness

The parser follows RFC 9112 strictly, because leniency is how request smuggling works when hws sits
behind or in front of another HTTP hop:

* `Content-Length` and `Transfer-Encoding` together: 400 and close.
* Conflicting or non-numeric `Content-Length`, duplicate `Transfer-Encoding`, codings other than a
  final `chunked`: 400 (or 501 for an unsupported coding).
* Whitespace before `:`, obsolete line folding, bare CR or LF, NUL and other control bytes: 400.
* Missing or duplicate `Host` on HTTP/1.1: 400.
* Any parse error answers with `Connection: close` and stops reading that connection, so bytes
  after the error are never interpreted as another request.
* Chunked bodies: bounded chunk extensions and trailers, overflow-checked sizes, strict CRLF.
* Limits: request head 16 KiB (431), request-target 8 KiB (414), 100 header fields (431),
  body 1 MiB (413, checked before reading when `Content-Length` is known).

## Static files and path safety

1. The raw path is percent-decoded and dot-segments are removed (`%2e%2e` is `..`). Climbing above
   the root is rejected with 400 rather than clamped.
2. Paths with a segment starting with `.` (`/.git/`, `/.env`) are 404 unless `serve-dotfiles` is on.
3. Files are opened with `openat2(root_fd, path, RESOLVE_BENEATH | RESOLVE_NO_MAGICLINKS)`, so the
   kernel refuses any resolution that leaves the root, including through symlinks and `/proc`
   magic links. `follow-symlinks=off` adds `RESOLVE_NO_SYMLINKS`. FIFOs and devices are refused.
4. The per-worker cache revalidates metadata with `fstatat` after `file-cache-ttl`; a changed
   inode, size or mtime reloads the entry, so atomic-rename deploys are picked up within the TTL.
   Missing files are cached too, so 404 floods do not turn into `openat2` floods.

Conditional requests (`If-None-Match` with weak comparison, `If-Modified-Since`), single byte
ranges (`206`, `416`, `If-Range`), directory redirects (`/docs` → `/docs/`) and index files are
supported. Multi-range requests get the full body, which RFC 9110 permits.

## Lifecycle

| Signal | Action |
|---|---|
| `SIGTERM` / `SIGINT` | Drain: each worker accepts what is already queued, closes its listeners, closes idle connections, lets in-flight responses finish with `Connection: close`, and exits when empty or at `drain-timeout`. `/healthz` answers 503 during the drain. A second signal stops immediately. |
| `SIGQUIT` | Stop immediately. |
| `SIGHUP` | Every worker reopens its access-log descriptor (log rotation). |

All lifecycle signals are blocked in every thread and consumed by the main thread with
`sigtimedwait`; commands reach workers through each loop's eventfd task queue.

**Overload.** Beyond `max-connections` (default: derived from `RLIMIT_NOFILE`, which hws raises to
its hard limit), new connections get an immediate `503` with `Retry-After`. If the process does run
out of descriptors, a reserved spare descriptor is released to accept and close the pending
connection, so the listen queue keeps moving instead of spinning on `EMFILE`.

**Restarts.** With `hws.socket` (systemd socket activation), systemd owns the listening socket, so
connections queue in the kernel across restarts instead of being refused. hws detects `LISTEN_FDS`
and switches to one shared socket with `EPOLLEXCLUSIVE`.

## Listener modes

| Mode | When | Trade-off |
|---|---|---|
| per-worker `SO_REUSEPORT` (default) | normal operation | Kernel spreads connections evenly by hash; no thundering herd. Connections still in a closing worker's accept queue are reset unless `net.ipv4.tcp_migrate_req=1`. |
| shared socket + `EPOLLEXCLUSIVE` | `--reuseport=off`, socket activation | One accept queue; survives restarts with socket activation. Under light load one idle worker tends to receive most new connections. |

## What it deliberately does not do

* **TLS.** Terminate TLS at a load balancer or sidecar (the common production layout). Keeping
  OpenSSL out of the worker loop keeps the hot path and the attack surface small.
* **HTTP/2 and HTTP/3**, WebSocket upgrades, reverse proxying, compression on the fly.
* **Streaming request bodies.** Bodies are buffered up to `max-body-bytes` before the handler runs.
* **Blocking handlers.** Handlers run on the event loop; anything that blocks stalls that worker.
