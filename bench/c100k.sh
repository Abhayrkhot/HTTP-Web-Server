#!/usr/bin/env bash
# Concurrency test: hold CONNS keep-alive connections, each sending a request every INTERVAL ms,
# against hws and then nginx, and report healthy connections, latency and server memory.
# Timeouts are matched to nginx's defaults (client_header_timeout 60s) because connhold spreads
# each connection's first request across the whole interval.
#
#   CONNS=200000 bench/c100k.sh
#
# Limits on a stock Linux box (no sysctl changes needed up to ~250k on loopback):
#   - ephemeral ports: ~28k per (source IP, destination) pair -> SOURCES spreads connections over
#     many 127.0.0.x source addresses
#   - RLIMIT_NOFILE hard limit (often 524288) caps each process
#   - nf_conntrack_max (if conntrack is loaded, loopback flows are tracked too)
set -euo pipefail

here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
repo=$(cd "$here/.." && pwd)
HWS_BIN=${HWS_BIN:-$repo/build/hws}
CONNHOLD=${CONNHOLD:-$repo/build/hws-connhold}
NGINX_BIN=${NGINX_BIN:-$HOME/tools/nginx/sbin/nginx}
CONNS=${CONNS:-200000}
SOURCES=${SOURCES:-127.0.0.2-127.0.0.21}
INTERVAL=${INTERVAL:-10000}
DURATION=${DURATION:-60000}
RATE=${RATE:-20000}
SERVER_CPUS=${SERVER_CPUS:-0-3}
CLIENT_CPUS=${CLIENT_CPUS:-4-11}
CLIENT_THREADS=${CLIENT_THREADS:-8}
SERVERS=${SERVERS:-"hws nginx"}
OUT=${OUT:-$repo/bench/results/raw/c100k-$(date +%Y%m%d-%H%M%S)}
PORT=18080

ulimit -n "$(ulimit -Hn)"
mkdir -p "$OUT"
work=$(mktemp -d)
trap 'cleanup' EXIT
cleanup() {
  [ -n "${server_pid:-}" ] && kill "$server_pid" 2>/dev/null || true
  [ -f "$work/nginx/nginx.pid" ] && kill "$(cat "$work/nginx/nginx.pid")" 2>/dev/null || true
  sleep 1
  rm -rf "$work"
}

WORKERS=$(python3 -c 'import sys
n=0
for p in sys.argv[1].split(","):
    a,_,b=p.partition("-"); n+=(int(b)-int(a)+1) if b else 1
print(n)' "$SERVER_CPUS")

wait_port() {
  for _ in $(seq 1 100); do
    (exec 3<>"/dev/tcp/127.0.0.1/$PORT") 2>/dev/null && return 0
    sleep 0.05
  done
  return 1
}

# Closed connections linger in conntrack (TIME_WAIT ~120s); wait so the next run gets the full
# table instead of overflowing it.
baseline_ct=$(cat /proc/sys/net/netfilter/nf_conntrack_count 2>/dev/null || echo 0)
settle() {
  [ -r /proc/sys/net/netfilter/nf_conntrack_count ] || { sleep 5; return; }
  for _ in $(seq 1 180); do  # up to 15 minutes
    [ "$(cat /proc/sys/net/netfilter/nf_conntrack_count)" -le $((baseline_ct + 5000)) ] && return
    sleep 5
  done
}

run_client() {  # label pids
  echo "== $1: $CONNS connections, 1 request per ${INTERVAL}ms each, ${DURATION}ms" | tee -a "$OUT/summary.txt"
  taskset -c "$CLIENT_CPUS" "$CONNHOLD" --port $PORT --path /hello --connections "$CONNS" --sources "$SOURCES" \
    --interval "$INTERVAL" --duration "$DURATION" --connect-rate "$RATE" --threads "$CLIENT_THREADS" \
    --server-pid "$2" > "$OUT/$1.txt" 2>&1 || true
  grep -E "^(SUMMARY|LATENCY_MS|SERVER_RSS_MB)" "$OUT/$1.txt" | tee -a "$OUT/summary.txt"
}

echo "# host=$(hostname) kernel=$(uname -r) conns=$CONNS server_cpus=$SERVER_CPUS workers=$WORKERS" \
  "nofile=$(ulimit -n) conntrack_max=$(cat /proc/sys/net/netfilter/nf_conntrack_max 2>/dev/null || echo n/a)" \
  | tee "$OUT/summary.txt"

for s in $SERVERS; do
  case $s in
    hws)
      taskset -c "$SERVER_CPUS" "$HWS_BIN" --listen "127.0.0.1:$PORT" --workers "$WORKERS" --pin-workers \
        --hello-path /hello --keepalive-timeout 120s --header-timeout 60s --max-requests-per-connection 0 \
        --log-level warn \
        > "$OUT/hws.log" 2>&1 &
      server_pid=$!
      wait_port
      sleep 1
      run_client hws "$server_pid"
      kill "$server_pid"; wait "$server_pid" 2>/dev/null || true; server_pid=
      ;;
    nginx)
      mkdir -p "$work/nginx/logs" "$work/www"
      per_worker=$(( CONNS / WORKERS * 13 / 10 + 1024 ))
      mask=$(python3 -c 'import sys
m=0
for p in sys.argv[1].split(","):
    a,_,b=p.partition("-")
    for c in range(int(a), int(b or a)+1): m|=1<<c
print(format(m,"b"))' "$SERVER_CPUS")
      sed -e "s#@WORKERS@#$WORKERS#" -e "s#@CPUMASK@#$mask#" -e "s#@PREFIX@#$work/nginx#" \
          -e "s#@ADDR@#127.0.0.1#" -e "s#@PORT@#$PORT#" -e "s#@ROOT@#$work/www#" \
          -e "s#worker_connections 262144#worker_connections $per_worker#" \
          -e "s#keepalive_timeout 65s#keepalive_timeout 120s#" \
          "$here/nginx.conf.template" > "$work/nginx/nginx.conf"
      "$NGINX_BIN" -p "$work/nginx" -c "$work/nginx/nginx.conf"
      wait_port
      sleep 1
      pids=$(pgrep -P "$(cat "$work/nginx/nginx.pid")" | paste -sd, -)
      run_client nginx "$(cat "$work/nginx/nginx.pid"),$pids"
      kill "$(cat "$work/nginx/nginx.pid")"
      sleep 1
      ;;
  esac
  settle
done
echo "results in $OUT"
