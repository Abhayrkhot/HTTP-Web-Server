# hws — a high-performance HTTP/1.1 server in C++20

[![ci](https://github.com/Abhayrkhot/HTTP-Web-Server/actions/workflows/ci.yml/badge.svg)](https://github.com/Abhayrkhot/HTTP-Web-Server/actions/workflows/ci.yml)

hws is an event-driven web server for Linux built on `epoll` and a shared-nothing multi-reactor
design: one event loop per core, each with its own `SO_REUSEPORT` listener, connections, timers,
file cache and metrics. It serves static files and in-process handlers, and is built to run in
production: strict HTTP parsing, timeouts on every state, graceful draining, Prometheus metrics,
systemd and container packaging, and a test suite that runs under ASan, UBSan, TSan and libFuzzer.

## Results

Measured on one Linux laptop (Core i7-12700H), every server pinned to the same 4 CPUs, against
nginx 1.30.7 configured for production. Medians of three interleaved rounds; method, every table
and the raw output are in [docs/BENCHMARKS.md](docs/BENCHMARKS.md).

| | previous version | **hws** | nginx 1.30.7 |
|---|---:|---:|---:|
| keep-alive req/s, in-memory response | 85k (19% of nginx) | **570k (125%)** | 457k |
| keep-alive req/s, 1 KiB static file | — | **474k (132%)** | 358k |
| req/s with 16-deep pipelining | fails | **4.88M (860%)** | 567k |
| p99 / p99.9 latency at a fixed 300k req/s | — | **2.9 / 6.4 ms** | 3.1 / 8.7 ms |
| concurrent keep-alive connections held | 9,986 (hard cap) | **200,000** | 200,000 |
| server memory at 200k connections | — | **56.5 MB** | 215.6 MB |
| syscalls per small-file request | — | **2** | 5 |

Large files (1 MiB, `sendfile`) and one-connection-per-request traffic tie with nginx: in both
cases the load generator, not either server, was the bottleneck. 200k connections is this
machine's limit (connection tracking and descriptor limits), not hws's.

## Features

**Performance**
* One epoll loop per worker thread, edge-triggered, one registration per connection and no
  `epoll_ctl` on the request path
* Per-worker `SO_REUSEPORT` listeners (or one shared socket with `EPOLLEXCLUSIVE`), optional CPU pinning
* Zero-copy request parsing into `string_view`s; pipelined responses coalesced into one `send()`
* Idle keep-alive connections hold no heap buffers (264-byte connection object)
* Small files served from memory, large files with `sendfile(2)`; per-worker open-file cache
* Hashed timing wheel: O(1) timeouts with lazy rescheduling

**HTTP/1.1**
* Keep-alive, pipelining, `Expect: 100-continue`, chunked request bodies, HTTP/1.0
* Static files: `ETag`/`If-None-Match`, `Last-Modified`/`If-Modified-Since`, byte ranges
  (`206`, `416`, `If-Range`), directory index and trailing-slash redirects, MIME types
* Router for in-process handlers (exact and prefix routes, `405` with `Allow`)

**Safety**
* Strict RFC 9112 parsing that rejects request-smuggling vectors (`CL`+`TE`, conflicting lengths,
  obs-fold, bare CR/LF, whitespace before `:`), with `Connection: close` on every parse error
* Limits on header size, header count, URI length and body size (`431`/`414`/`413`)
* Timeouts for headers (slowloris), bodies, idle keep-alive and slow readers
* Path traversal blocked twice: normalization rejects `..` above root, and `openat2(RESOLVE_BENEATH)`
  makes the kernel refuse anything outside the document root, symlinks included
* Dotfiles hidden by default; response headers validated against CR/LF injection
* Backpressure on pipelined floods; `503` beyond `max-connections`; `EMFILE` shedding

**Operations**
* `SIGTERM` drains gracefully (in-flight responses finish, `/healthz` returns 503); `SIGHUP`
  reopens logs; systemd socket activation for zero-downtime restarts
* Prometheus `/metrics`: connections (per worker), responses by class, latency histogram,
  timeouts, parse errors, bytes, file-cache hit rate, RSS
* Buffered access log in Combined Log Format with request time
* Config file and flags for everything; hardened systemd unit; distroless container image

## Quick start

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/hws --listen 0.0.0.0:8080 --root ./public --access-log stdout
```

```bash
curl -i http://localhost:8080/
curl http://localhost:8080/metrics
```

Requirements: Linux 5.6+ (for `openat2`; older kernels fall back to `openat` with a warning),
GCC 12+ or Clang 16+, CMake 3.20+. GoogleTest is fetched automatically when tests are enabled.

### Container

```bash
docker build -f deploy/Dockerfile -t hws .
docker run -p 8080:8080 -v "$PWD/public:/srv/www:ro" hws
```

### systemd

[`deploy/hws.service`](deploy/hws.service) runs hws sandboxed (`DynamicUser`, `ProtectSystem=strict`,
syscall filter, no capabilities) and [`deploy/hws.socket`](deploy/hws.socket) hands it the listening
socket, so restarts never refuse connections.

## Configuration

Every option works as a flag (`--name value` or `--name=value`) and as a `name = value` line in a
config file (`--config FILE`; flags override the file). See [`deploy/hws.conf`](deploy/hws.conf)
and `hws --help`.

| Option | Default | |
|---|---|---|
| `listen` | `0.0.0.0:8080` | comma-separated; `[::]:8080` for IPv6 |
| `workers` | CPUs | one event loop per worker |
| `reuseport` | `on` | `off`: one shared socket with `EPOLLEXCLUSIVE` |
| `root` | — | static files; unset disables them |
| `max-connections` | from `RLIMIT_NOFILE` | `503` beyond this |
| `keepalive-timeout` / `header-timeout` / `body-timeout` / `write-timeout` | 60s / 10s / 30s / 30s | |
| `drain-timeout` | 30s | graceful shutdown deadline |
| `max-header-bytes` / `max-uri-bytes` / `max-body-bytes` | 16k / 8k / 1m | |
| `file-cache-ttl` | 2s | how quickly changes on disk become visible |
| `inline-file-max` | 16k | files up to this size are served from memory |
| `access-log` | `off` | `stdout` or a file path |
| `health-path` / `metrics-path` | `/healthz` / `/metrics` | empty disables |

## Embedding handlers

```cpp
hws::Server server(cfg);
server.init(err);
server.router().add("/api/time", hws::http::kGet, [](const hws::http::Request& req, hws::http::Response& resp) {
  resp.set_content_type("application/json");
  resp.set_body("{\"unix\":" + std::to_string(time(nullptr)) + "}");
});
return server.run();
```

Handlers run on the worker's event loop and must not block. Exceptions become `500`.

## Testing

```bash
ctest --test-dir build --output-on-failure        # 68 unit tests + 35 integration tests
cmake -B build-asan -DHWS_SANITIZE=address,undefined && cmake --build build-asan -j && ctest --test-dir build-asan
cmake -B build-tsan -DHWS_SANITIZE=thread && cmake --build build-tsan -j && ctest --test-dir build-tsan
docker build -t hws-fuzz fuzz/ && docker run --rm -v "$PWD:/src" -w /src hws-fuzz fuzz/run.sh 300
```

* **Unit tests** (GoogleTest): parser and smuggling vectors, chunked decoding, path normalization,
  ranges and ETags, HTTP dates, timer wheel, buffers, router, config.
* **Integration tests** (Python, raw sockets, against the real binary): static files, conditional
  and range requests, traversal and symlink escapes, 8 MiB sendfile downloads, pipelining (also
  byte-by-byte), chunked uploads, `100-continue`, limits, smuggling attempts, slowloris, idle and
  write timeouts, `max-connections`, access-log rotation, graceful drain mid-download, drain
  deadline, second-signal stop. The harness fails a test if the server prints any sanitizer report.
* **Fuzzing** (libFuzzer + ASan/UBSan): the request parser over pipelined streams, the chunked
  decoder (differential: byte-split vs one-shot), path normalization (no result may escape root or
  keep dot segments), range/date/config parsers. A 5-minute-per-target run executed 335 million
  inputs (parser 34M, chunked 108M, uri 39M, misc 155M) with no findings; CI fuzzes every push.
* **CI** runs all of the above on GCC 14 and Clang 18, plus a Docker image smoke test.

## Documentation

* [Architecture](docs/ARCHITECTURE.md): threading model, connection state machine, hot path,
  memory, fairness, protocol strictness, lifecycle
* [Benchmarks](docs/BENCHMARKS.md): methodology, results, how to reproduce

## Layout

```
src/util     buffers, timer wheel, logging, RAII fds
src/net      epoll event loop, listener sockets
src/http     parser, chunked decoder, URI normalization, ranges, dates, router, responses
src/server   connections, workers, server lifecycle, static files, file cache, metrics, access log
tests/       GoogleTest unit tests, Python integration tests
fuzz/        libFuzzer targets, seed corpus, dictionary
bench/       wrk/nginx comparison and concurrency scripts, recorded results
tools/       hws-connhold, a load tool for holding hundreds of thousands of connections
deploy/      Dockerfile, systemd units, example config
```
