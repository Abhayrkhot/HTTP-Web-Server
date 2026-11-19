# Benchmarks

All numbers on this page were measured with the scripts in [`bench/`](../bench), and the raw
output of every run is kept under [`bench/results/`](../bench/results). Nothing here is estimated.

## Results at a glance

| | previous server | **hws** | nginx 1.30.7 |
|---|---:|---:|---:|
| `/hello` keep-alive, req/s | 85,118 (19%) | **570,234 (125%)** | 457,488 |
| 1 KiB file, req/s | — | **473,913 (132%)** | 358,030 |
| 16 KiB file, req/s | — | **357,850 (110%)** | 323,981 |
| 1 MiB file, req/s (client-bound) | — | 20,273 (99%) | 20,543 |
| 16-deep pipelining, req/s | fails | **4,875,932 (860%)** | 566,712 |
| `Connection: close`, req/s (client-bound) | 86,511 (61%) | 142,516 (100%) | 141,846 |
| p99 / p99.9 at 300k req/s | — | **2.90 / 6.39 ms** | 3.08 / 8.70 ms |
| concurrent keep-alive connections | 9,986 (hard cap) | **200,000** | 200,000 |
| server RSS at 200k connections | — | **56.5 MB** | 215.6 MB |

Percentages are relative to nginx. All three servers ran on the same 4 pinned CPUs.

## Setup

| | |
|---|---|
| Machine | Laptop, Intel Core i7-12700H (6 P-cores + 8 E-cores, 20 threads), 61 GB RAM |
| OS | Ubuntu 26.04, Linux 7.0.0-31-generic |
| Compiler | GCC 15.2, `-O3` Release build with LTO (`cmake -DCMAKE_BUILD_TYPE=Release`) |
| hws | 2.0.0 (this repository) |
| nginx | 1.30.7 (current stable), built from source with `-O2` |
| Load generators | wrk 4.2.0, wrk2 (constant-rate latency), `hws-connhold` (this repo, for concurrency) |
| Network | loopback (`127.0.0.1`) |

**Isolation.** The server under test is pinned to CPUs 0-3 (two P-cores with their hyperthreads)
and gets one worker per CPU (`--workers 4 --pin-workers` for hws, `worker_processes 4` with
`worker_cpu_affinity` for nginx). wrk runs on CPUs 4-11 (four other P-cores) with 8 threads. The
two never share a core.

**Fairness rules.**

* Both servers serve the same files from the same directory.
* nginx is configured the way it would be in production for this workload
  ([`bench/nginx.conf.template`](../bench/nginx.conf.template)): `reuseport`, `epoll`,
  `multi_accept`, `sendfile`, `tcp_nopush`, `tcp_nodelay`, `open_file_cache`, `access_log off`.
* Both servers have access logging off.
* Keep-alive connections are never recycled mid-run (`keepalive_requests` raised for nginx,
  `--max-requests-per-connection 0` for hws).
* Runs are interleaved (hws, nginx, hws, nginx, ...) so background load on the shared machine hits
  both equally, and each run is preceded by a 2-second warm-up.
* Every scenario runs three rounds; tables report the median and list every round.

**Caveat.** The box is a shared development laptop that was running other workloads (Kubernetes
lab clusters) during measurement, so absolute numbers carry a few percent of noise. The
interleaving is there so that noise does not favor either server; compare ratios, not absolutes.

## Reproduce

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
bench/run.sh                       # throughput scenarios, hws vs nginx
OLD_BIN=/path/to/httpcgiservermain bench/run.sh    # also the pre-rewrite server
CONNS=200000 bench/c100k.sh        # concurrency + memory
```

`bench/run.sh` expects `wrk` and `nginx` in `~/tools` by default (override with `WRK=`,
`NGINX_BIN=`).

## Throughput and latency

Medians of three interleaved 15-second rounds (all three rounds listed), 256 keep-alive connections from 8 wrk threads unless noted. CPU columns are the share of one CPU used during the run (400% = all four server CPUs). Raw wrk output for every run: [`bench/results/2026-11-19-throughput/`](../bench/results/2026-11-19-throughput).

### In-memory 13-byte response (`/hello`), keep-alive

| server | req/s (median) | vs nginx | rounds | p50 | p99 | server CPU | wrk CPU |
|---|---:|---:|---|---:|---:|---:|---:|
| **hws** | 570,234 | 125% | 616,795, 570,234, 509,413 | 454 µs | 614 µs | 397% | 486% |
| nginx | 457,488 | 100% | 513,454, 457,488, 453,521 | 539 µs | 771 µs | 398% | 436% |
| previous server | 85,118 | 19% | 85,118, 84,371, 86,779 | 3.30 ms | 4.03 ms | 244% | 72% |

Both serve the same 13-byte body (`Hello, World!`); hws's headers are a little smaller (119 bytes per response on the wire vs 150 for nginx, nginx adds `Connection: keep-alive` and a longer `Server: nginx/1.30.7`). hws does more work per CPU second: both are pinned at ~397% of their 4 CPUs while wrk still has headroom. The previous server (measured on its `/cgi-bin/example` page, its only dynamic route) tops out at 85k req/s on 2.4 cores (one acceptor thread plus three I/O threads), 19% of nginx.

### 1 KiB static file, keep-alive

| server | req/s (median) | vs nginx | rounds | p50 | p99 | server CPU | wrk CPU |
|---|---:|---:|---|---:|---:|---:|---:|
| **hws** | 473,913 | 132% | 474,394, 472,778, 473,913 | 533 µs | 785 µs | 397% | 473% |
| nginx | 358,030 | 100% | 358,030, 359,420, 357,595 | 698 µs | 1.04 ms | 397% | 344% |

The widest gap on real files: hws serves the body from its per-worker in-memory cache in the same `send` as the headers (2 syscalls per request), while nginx uses `writev` + `sendfile` and toggles `TCP_CORK` (5 syscalls). See [syscalls per request](#syscalls-per-request).

### 16 KiB static file, keep-alive

| server | req/s (median) | vs nginx | rounds | p50 | p99 | server CPU | wrk CPU |
|---|---:|---:|---|---:|---:|---:|---:|
| **hws** | 357,850 | 110% | 358,432, 357,850, 356,588 | 733 µs | 1.05 ms | 396% | 481% |
| nginx | 323,981 | 100% | 323,981, 324,859, 322,788 | 775 µs | 1.04 ms | 397% | 398% |

Still under `inline-file-max` (16 KiB), so hws still answers from memory; copying 16 KiB per response narrows the gap.

### 1 MiB static file, keep-alive (sendfile)

| server | req/s (median) | vs nginx | rounds | p50 | p99 | server CPU | wrk CPU |
|---|---:|---:|---|---:|---:|---:|---:|
| **hws** | 20,273 | 99% | 20,153, 20,273, 20,451 | 6.41 ms | 12.67 ms | 147% | 795% |
| nginx | 20,543 | 100% | 20,543, 20,518, 20,687 | 6.34 ms | 12.67 ms | 162% | 796% |

**A tie, and the load generator is the bottleneck**: wrk is at 796% of its 8 CPUs (about 20 GiB/s of loopback traffic). Both servers use `sendfile`; hws needs 9% less CPU for the same throughput.

### `/hello`, 16 pipelined requests per round trip

| server | req/s (median) | vs nginx | rounds | p50 | p99 | server CPU | wrk CPU |
|---|---:|---:|---|---:|---:|---:|---:|
| **hws** | 4,875,932 | 860% | 4,278,569, 4,939,934, 4,875,932 | n/a | n/a | 397% | 390% |
| nginx | 566,712 | 100% | 523,056, 566,712, 571,296 | n/a | n/a | 398% | 293% |
| previous server | fails | | read errors on all 256 connections | n/a | n/a | 1% | 0% |

The TechEmpower "plaintext" pattern. hws parses every request in a read and answers the whole batch with one `send` (0.13 syscalls per request); nginx writes each response separately (1.07 syscalls per request: 609,841 `writev` calls for 609,688 requests under strace), so pipelining barely helps it. Response sizes check out (119 bytes per response from hws pipelined or not), so every counted response is complete. The previous server fails under pipelining: all 256 connections error out. wrk times only one request per pipelined batch, so latency is not shown.

### `/hello`, new connection per request (`Connection: close`)

| server | req/s (median) | vs nginx | rounds | p50 | p99 | server CPU | wrk CPU |
|---|---:|---:|---|---:|---:|---:|---:|
| **hws** | 142,516 | 100% | 163,658, 141,562, 142,516 | 564 µs | 6.39 ms | 357% | 782% |
| nginx | 141,846 | 100% | 149,595, 141,846, 141,814 | 602 µs | 6.68 ms | 365% | 780% |
| previous server | 86,511 | 61% | 86,511, 86,628, 86,054 | 3.27 ms | 4.34 ms | 245% | 72% |

**A tie, and the load generator is the bottleneck** (wrk at 782% of 8 CPUs creating TCP connections); neither server is saturated. The previous server reaches 61% of nginx.

### Latency at a fixed rate (wrk2, coordinated-omission corrected)

| server | achieved req/s | p50 | p99 | p99.9 | p99.99 | server CPU |
|---|---:|---:|---:|---:|---:|---:|
| **hws** | 299,209 | 1.16 ms | 2.90 ms | 6.39 ms | 8.97 ms | 199% |
| nginx | 299,202 | 1.20 ms | 3.08 ms | 8.70 ms | 11.44 ms | 231% |

300,000 req/s of `/hello` over 256 connections, 30 s per round, medians of three rounds per percentile. That is ~60% of nginx's capacity and ~50% of hws's. hws's tail is lower (p99.9 6.4 ms vs 8.7 ms) while it uses 14% less CPU for the same load.

## Concurrency

`bench/c100k.sh` opens 200,000 keep-alive connections (from 20 loopback source addresses, ramping
at 20,000/s) and keeps every one of them busy with one request every 10 seconds for a minute, so
about 20,000 requests/s are spread across 200,000 sockets. Same 4 pinned CPUs per server. Raw
output: [`bench/results/2026-11-19-c200k/`](../bench/results/2026-11-19-c200k).

| | connections held | requests | errors | p50 | p99 | p99.9 | server RSS at 200k |
|---|---:|---:|---:|---:|---:|---:|---:|
| **hws** | **200,000 / 200,000** | 1,100,130 | 0 | 119 µs | 563 µs | 1.61 ms | **56.5 MB** |
| nginx 1.30.7 | 200,000 / 200,000 | 1,100,150 | 0 | 121 µs | 540 µs | 1.30 ms | 215.6 MB |
| previous server | **9,986 / 15,000** | 57,858 | 5,014 dropped | 174 µs | 794 µs | 2.00 ms | 54.2 MB at 9,986 |

* hws grows by **0.27 KB of RSS per connection** (4.6 MB idle → 56.5 MB at 200k, linear the
  whole way). An idle connection is a 264-byte object with no buffers attached. nginx grows by
  0.52 KB per connection on top of the connection arrays it preallocates for `worker_connections`
  (here sized to the test, 66k per worker).
* Latency is the same for both: at this request rate neither server is CPU-bound.
* The previous server closes any socket whose descriptor number exceeds 10,000, so it can never
  hold more than ~10,000 connections regardless of hardware, and it spends 5.1 KB of RSS per
  connection (19× hws) on buffers it allocates per socket
  ([`bench/old-c10k.sh`](../bench/old-c10k.sh), 15,000 attempted, 1 request every 5 s;
  [raw output](../bench/results/2026-11-19-old-c10k/old.txt)).
* 200,000 is the limit of this machine, not of hws: loopback flows count against
  `nf_conntrack_max` (262,144 here, changing it needs root), and each process is capped by an
  `RLIMIT_NOFILE` hard limit of 524,288. Both client and server ends of every connection live on
  the same host.

## Syscalls per request

Measured with `strace -f -c` while wrk drove 32 keep-alive connections for 5 seconds; total
syscalls divided by requests served. strace slows both servers equally, so only the ratio matters.

| request | hws | nginx | nginx breakdown |
|---|---:|---:|---|
| `/hello` (in-memory) | **2.07** | 2.07 | `recvfrom`, `writev` |
| 1 KiB static file | **2.07** | 5.07 | `recvfrom`, `writev` (headers), `sendfile` (body), 2 × `setsockopt` (`TCP_CORK` on/off for `tcp_nopush`) |

hws answers a small-file request with one `read` and one `send`: the body comes from the
per-worker in-memory file cache and is written in the same `send` as the headers. The remaining
0.07 per request is `epoll_wait`, amortized because one wakeup returns many ready connections.
This is most of why hws is ahead on small static files below. For large files both servers use
`sendfile`.
