    # host=llm-server kernel=7.0.0-31-generic cpu="12th Gen Intel(R) Core(TM) i7-12700H"
    # server_cpus=0-3 client_cpus=4-11 workers=4 conns=256 threads=8 duration=15s rounds=3
    # hws=hws 2.0.0 nginx=1.30.7
    # loadavg_before=2.61 2.90 2.84
    # loadavg_after=8.64 8.10 6.45

### In-memory 13-byte response (`/hello`), keep-alive

| server | req/s (median) | vs nginx | rounds | p50 | p99 | server CPU | wrk CPU |
|---|---:|---:|---|---:|---:|---:|---:|
| **hws** | 570,234 | 125% | 616,795, 570,234, 509,413 | 454 µs | 614 µs | 397% | 486% |
| nginx | 457,488 | 100% | 513,454, 457,488, 453,521 | 539 µs | 771 µs | 398% | 436% |
| previous server | 85,118 | 19% | 85,118, 84,371, 86,779 | 3.30 ms | 4.03 ms | 244% | 72% |

### 1 KiB static file, keep-alive

| server | req/s (median) | vs nginx | rounds | p50 | p99 | server CPU | wrk CPU |
|---|---:|---:|---|---:|---:|---:|---:|
| **hws** | 473,913 | 132% | 474,394, 472,778, 473,913 | 533 µs | 785 µs | 397% | 473% |
| nginx | 358,030 | 100% | 358,030, 359,420, 357,595 | 698 µs | 1.04 ms | 397% | 344% |

### 16 KiB static file, keep-alive

| server | req/s (median) | vs nginx | rounds | p50 | p99 | server CPU | wrk CPU |
|---|---:|---:|---|---:|---:|---:|---:|
| **hws** | 357,850 | 110% | 358,432, 357,850, 356,588 | 733 µs | 1.05 ms | 396% | 481% |
| nginx | 323,981 | 100% | 323,981, 324,859, 322,788 | 775 µs | 1.04 ms | 397% | 398% |

### 1 MiB static file, keep-alive (sendfile)

| server | req/s (median) | vs nginx | rounds | p50 | p99 | server CPU | wrk CPU |
|---|---:|---:|---|---:|---:|---:|---:|
| **hws** | 20,273 | 99% | 20,153, 20,273, 20,451 | 6.41 ms | 12.67 ms | 147% | 795% |
| nginx | 20,543 | 100% | 20,543, 20,518, 20,687 | 6.34 ms | 12.67 ms | 162% | 796% |

### `/hello`, 16 pipelined requests per round trip

| server | req/s (median) | vs nginx | rounds | p50 | p99 | server CPU | wrk CPU |
|---|---:|---:|---|---:|---:|---:|---:|
| **hws** | 4,875,932 | 860% | 4,278,569, 4,939,934, 4,875,932 | n/a | n/a | 397% | 390% |
| nginx | 566,712 | 100% | 523,056, 566,712, 571,296 | n/a | n/a | 398% | 293% |
| previous server | 34 | 0% | 34, 34, 34 | n/a | n/a | 1% | 0% |
|  | ⚠ Socket errors: connect 0, read 256, write 0, timeout 0 | | | | | | |

### `/hello`, new connection per request (`Connection: close`)

| server | req/s (median) | vs nginx | rounds | p50 | p99 | server CPU | wrk CPU |
|---|---:|---:|---|---:|---:|---:|---:|
| **hws** | 142,516 | 100% | 163,658, 141,562, 142,516 | 564 µs | 6.39 ms | 357% | 782% |
| nginx | 141,846 | 100% | 149,595, 141,846, 141,814 | 602 µs | 6.68 ms | 365% | 780% |
| previous server | 86,511 | 61% | 86,511, 86,628, 86,054 | 3.27 ms | 4.34 ms | 245% | 72% |

### Latency at a fixed rate (wrk2, coordinated-omission corrected)

| server | achieved req/s | p50 | p99 | p99.9 | p99.99 | server CPU |
|---|---:|---:|---:|---:|---:|---:|
| **hws** | 299,209 | 1.16 ms | 2.90 ms | 6.39 ms | 8.97 ms | 199% |
| nginx | 299,202 | 1.20 ms | 3.08 ms | 8.70 ms | 11.44 ms | 231% |

