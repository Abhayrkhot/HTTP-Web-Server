#!/usr/bin/env bash
# Throughput/latency comparison: hws vs nginx (and optionally the pre-rewrite server) on one
# Linux host over loopback. Servers and load generator are pinned to disjoint CPU sets so they
# do not steal cycles from each other, and runs are interleaved server-by-server per round so
# background noise hits every contender equally.
#
#   bench/run.sh                      # all scenarios (latency uses wrk2 at LAT_RATE req/s), 3 rounds
#   ROUNDS=5 SCENARIOS="hello file1k" bench/run.sh
#
# Environment:
#   HWS_BIN      path to hws (default build/hws)
#   NGINX_BIN    path to nginx (default ~/tools/nginx/sbin/nginx)
#   OLD_BIN      optional path to the pre-rewrite server binary (port 9508, /cgi-bin/example)
#   WRK / WRK2   wrk and wrk2 binaries (default ~/tools/bin/wrk{,2})
#   SERVER_CPUS  CPUs for the server under test (default 0-3)
#   CLIENT_CPUS  CPUs for wrk (default 4-11)
#   DURATION     seconds per run (default 15)
set -euo pipefail

here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
repo=$(cd "$here/.." && pwd)
HWS_BIN=${HWS_BIN:-$repo/build/hws}
NGINX_BIN=${NGINX_BIN:-$HOME/tools/nginx/sbin/nginx}
OLD_BIN=${OLD_BIN:-}
WRK=${WRK:-$HOME/tools/bin/wrk}
WRK2=${WRK2:-$HOME/tools/bin/wrk2}
SERVER_CPUS=${SERVER_CPUS:-0-3}
CLIENT_CPUS=${CLIENT_CPUS:-4-11}
DURATION=${DURATION:-15}
ROUNDS=${ROUNDS:-3}
SCENARIOS=${SCENARIOS:-"hello file1k file16k file1m pipeline close latency"}
WRK_THREADS=${WRK_THREADS:-8}
CONNS=${CONNS:-256}
OUT=${OUT:-$repo/bench/results/raw/$(date +%Y%m%d-%H%M%S)}
PORT_HWS=18080
PORT_NGINX=18081
ADDR=127.0.0.1

ulimit -n "$(ulimit -Hn)"
mkdir -p "$OUT"
work=$(mktemp -d)
trap 'stop_all; rm -rf "$work"' EXIT

ncpus() {  # number of CPUs in a taskset list like 0-3,8
  python3 -c 'import sys
n=0
for p in sys.argv[1].split(","):
    a,_,b=p.partition("-"); n+= (int(b)-int(a)+1) if b else 1
print(n)' "$1"
}
cpumask() {  # nginx worker_cpu_affinity mask covering the list
  python3 -c 'import sys
m=0
for p in sys.argv[1].split(","):
    a,_,b=p.partition("-")
    for c in range(int(a), int(b or a)+1): m|=1<<c
print(format(m,"b"))' "$1"
}
WORKERS=$(ncpus "$SERVER_CPUS")

# Document root shared by both servers.
root=$work/www
mkdir -p "$root"
head -c 1024 /dev/urandom | base64 -w0 | head -c 1024 > "$root/1k.html"
head -c $((16 * 1024)) /dev/urandom > "$root/16k.bin"
head -c $((1024 * 1024)) /dev/urandom > "$root/1m.bin"

pids=()
stop_all() {
  for p in "${pids[@]:-}"; do [ -n "$p" ] && kill "$p" 2>/dev/null || true; done
  [ -f "$work/nginx/nginx.pid" ] && kill "$(cat "$work/nginx/nginx.pid")" 2>/dev/null || true
  pids=()
  sleep 0.5
}

wait_port() {
  for _ in $(seq 1 100); do
    (exec 3<>"/dev/tcp/$ADDR/$1") 2>/dev/null && return 0
    sleep 0.05
  done
  echo "server on port $1 did not come up" >&2
  return 1
}

start_hws() {
  taskset -c "$SERVER_CPUS" "$HWS_BIN" --listen "$ADDR:$PORT_HWS" --workers "$WORKERS" --pin-workers \
    --root "$root" --hello-path /hello --max-requests-per-connection 0 --log-level warn \
    >"$OUT/hws.log" 2>&1 &
  pids+=($!)
  wait_port $PORT_HWS
}

start_nginx() {
  mkdir -p "$work/nginx/logs"
  sed -e "s#@WORKERS@#$WORKERS#" -e "s#@CPUMASK@#$(cpumask "$SERVER_CPUS")#" -e "s#@PREFIX@#$work/nginx#" \
      -e "s#@ADDR@#$ADDR#" -e "s#@PORT@#$PORT_NGINX#" -e "s#@ROOT@#$root#" \
      "$here/nginx.conf.template" > "$work/nginx/nginx.conf"
  "$NGINX_BIN" -p "$work/nginx" -c "$work/nginx/nginx.conf"
  wait_port $PORT_NGINX
}

start_old() {
  # The original server listens on 0.0.0.0:9508 with 3 IO threads and only serves CGI routes.
  (cd "$(dirname "$OLD_BIN")" && taskset -c "$SERVER_CPUS" "$OLD_BIN" >"$OUT/old.log" 2>&1) &
  pids+=($!)
  wait_port 9508
}

url_for() {  # server scenario
  local port path
  case $1 in
    hws) port=$PORT_HWS ;; nginx) port=$PORT_NGINX ;; old) port=9508 ;;
  esac
  case $2 in
    hello|pipeline|close|latency) path=/hello ;;
    file1k) path=/1k.html ;; file16k) path=/16k.bin ;; file1m) path=/1m.bin ;;
  esac
  [ "$1" = old ] && path=/cgi-bin/example
  echo "http://$ADDR:$port$path"
}

server_pids() {  # PIDs whose CPU time counts as "the server"
  case $1 in
    hws) echo "${pids[-1]}" ;;
    nginx) pgrep -P "$(cat "$work/nginx/nginx.pid")" | tr '\n' ' ' ;;
    old) pgrep -f '[h]ttpcgiservermain' | tr '\n' ' ' ;;
  esac
}

cpu_ticks() {  # utime+stime of the given processes (all threads), in clock ticks
  local total=0 p
  for p in "$@"; do
    [ -r "/proc/$p/stat" ] || continue
    total=$(( total + $(awk '{print $14 + $15}' "/proc/$p/stat") ))
  done
  echo "$total"
}

LAT_RATE=${LAT_RATE:-200000}
LAT_DURATION=${LAT_DURATION:-30}
CLK_TCK=$(getconf CLK_TCK)

run_wrk() {  # server scenario round
  local url log spids t0 t1 start end secs
  url=$(url_for "$1" "$2")
  log="$OUT/$2.$1.r$3.txt"
  spids=$(server_pids "$1")
  # shellcheck disable=SC2086
  t0=$(cpu_ticks $spids)
  start=$(date +%s.%N)
  # `times` inside the subshell reports wrk's CPU time as the subshell's children.
  (
    case $2 in
      pipeline)
        taskset -c "$CLIENT_CPUS" "$WRK" -t"$WRK_THREADS" -c"$CONNS" -d"${DURATION}s" --latency \
          -s "$here/pipeline.lua" "$url" -- 16 ;;
      close)
        taskset -c "$CLIENT_CPUS" "$WRK" -t"$WRK_THREADS" -c"$CONNS" -d"${DURATION}s" --latency \
          -H "Connection: close" "$url" ;;
      latency)
        taskset -c "$CLIENT_CPUS" "$WRK2" -t"$WRK_THREADS" -c"$CONNS" -d"${LAT_DURATION}s" -R"$LAT_RATE" \
          --latency "$url" ;;
      *)
        taskset -c "$CLIENT_CPUS" "$WRK" -t"$WRK_THREADS" -c"$CONNS" -d"${DURATION}s" --latency "$url" ;;
    esac
    times > "$log.times"  # not in a pipeline: line 2 is this subshell's children, i.e. wrk
  ) > "$log" 2>&1
  end=$(date +%s.%N)
  # shellcheck disable=SC2086
  t1=$(cpu_ticks $spids)
  secs=$(python3 -c "print(max(float('$end') - float('$start'), 0.001))")
  local server_cpu client_cpu rps
  server_cpu=$(python3 -c "print(round(($t1 - $t0) / $CLK_TCK / $secs * 100))")
  client_cpu=$(python3 -c 'import re,sys
t=re.findall(r"(\d+)m([\d.]+)s", open(sys.argv[1]).read().splitlines()[-1])
print(round(sum(int(m)*60+float(s) for m,s in t) / float(sys.argv[2]) * 100))' "$log.times" "$secs")
  rm -f "$log.times"
  echo "server_cpu_pct=$server_cpu client_cpu_pct=$client_cpu" >> "$log"
  rps=$(awk '/Requests\/sec/ {print $2}' "$log")
  if [ "$2" = latency ]; then
    printf '%-9s %-6s r%s %12s req/s  p50 %-8s p99 %-8s p99.9 %-8s p99.99 %-8s cpu srv %s%% cli %s%% %s\n' \
      "$2" "$1" "$3" "$rps" "$(awk '$1=="50.000%" {print $2}' "$log")" "$(awk '$1=="99.000%" {print $2}' "$log")" \
      "$(awk '$1=="99.900%" {print $2}' "$log")" "$(awk '$1=="99.990%" {print $2}' "$log")" \
      "$server_cpu" "$client_cpu" "$(grep -E 'Non-2xx|Socket errors' "$log" | tr '\n' ' ')" | tee -a "$OUT/summary.txt"
  else
    printf '%-9s %-6s r%s %12s req/s  p50 %-8s p99 %-8s cpu srv %s%% cli %s%% %s\n' "$2" "$1" "$3" "$rps" \
      "$(awk '$1=="50%" {print $2}' "$log")" "$(awk '$1=="99%" {print $2}' "$log")" "$server_cpu" "$client_cpu" \
      "$(grep -E 'Non-2xx|Socket errors' "$log" | tr '\n' ' ')" | tee -a "$OUT/summary.txt"
  fi
}

servers="hws nginx"
[ -n "$OLD_BIN" ] && servers="$servers old"

echo "# host=$(hostname) kernel=$(uname -r) cpu=\"$(lscpu | sed -n 's/Model name: *//p')\"" | tee "$OUT/summary.txt"
echo "# server_cpus=$SERVER_CPUS client_cpus=$CLIENT_CPUS workers=$WORKERS conns=$CONNS threads=$WRK_THREADS duration=${DURATION}s rounds=$ROUNDS" | tee -a "$OUT/summary.txt"
echo "# hws=$("$HWS_BIN" --version) nginx=$("$NGINX_BIN" -v 2>&1 | sed 's/.*\///')" | tee -a "$OUT/summary.txt"
echo "# loadavg_before=$(cut -d' ' -f1-3 /proc/loadavg)" | tee -a "$OUT/summary.txt"

for scenario in $SCENARIOS; do
  for round in $(seq 1 "$ROUNDS"); do
    for s in $servers; do
      [ "$s" = old ] && case $scenario in file*) continue ;; esac
      "start_$s"
      # warm-up: fill caches, let the CPU governor settle
      taskset -c "$CLIENT_CPUS" "$WRK" -t2 -c32 -d2s "$(url_for "$s" "$scenario")" >/dev/null 2>&1 || true
      run_wrk "$s" "$scenario" "$round"
      stop_all
    done
  done
done
echo "# loadavg_after=$(cut -d' ' -f1-3 /proc/loadavg)" | tee -a "$OUT/summary.txt"
echo "results in $OUT"
