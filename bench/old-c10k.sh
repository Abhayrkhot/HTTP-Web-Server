#!/usr/bin/env bash
# Concurrency check for the pre-rewrite server (the CGI server this repository held before hws): it
# closes any accepted socket whose descriptor number exceeds 10000 and listens with a backlog of 128.
#   OLD_BIN=/path/to/httpcgiservermain CONNS=15000 bench/old-c10k.sh
set -euo pipefail
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
repo=$(cd "$here/.." && pwd)
CONNHOLD=${CONNHOLD:-$repo/build/hws-connhold}
CONNS=${CONNS:-15000}
ulimit -n "$(ulimit -Hn)"
(cd "$(dirname "$OLD_BIN")" && taskset -c "${SERVER_CPUS:-0-3}" "$OLD_BIN" >/dev/null 2>&1) &
sleep 1
pid=$(pgrep -x httpcgiserverma | head -1)  # comm is truncated to 15 chars
taskset -c "${CLIENT_CPUS:-4-11}" "$CONNHOLD" --port 9508 --path /cgi-bin/example --connections "$CONNS" \
  --sources 127.0.0.2-127.0.0.3 --interval 5000 --duration 30000 --connect-rate 5000 --threads 4 \
  --server-pid "$pid" || true
kill "$pid" 2>/dev/null || true
