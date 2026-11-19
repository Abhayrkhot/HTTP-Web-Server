#!/usr/bin/env bash
# Builds every fuzz target with ASan+UBSan and runs each for SECONDS (default 120), starting from
# the checked-in seed corpus. Findings land in fuzz/findings/<target>/.
set -euo pipefail
seconds=${1:-120}
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
repo=$(cd "$here/.." && pwd)
build=${BUILD_DIR:-$repo/build-fuzz}
cmake -S "$repo" -B "$build" -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_CXX_COMPILER=clang++ \
  -DHWS_BUILD_FUZZERS=ON -DHWS_SANITIZE=address,undefined -DHWS_BUILD_TESTS=OFF -DHWS_BUILD_TOOLS=OFF >/dev/null
cmake --build "$build" -j"$(nproc)" --target fuzz_parser fuzz_chunked fuzz_uri fuzz_misc >/dev/null
status=0
for t in parser chunked uri misc; do
  work=$(mktemp -d)
  cp -r "$here/corpus/$t/." "$work/"
  mkdir -p "$here/findings/$t"
  echo "== fuzz_$t for ${seconds}s"
  if ! "$build/fuzz/fuzz_$t" "$work" -max_total_time="$seconds" -timeout=10 -rss_limit_mb=2048 \
        -dict="$here/http.dict" -print_final_stats=1 -artifact_prefix="$here/findings/$t/" 2>"$work.log"; then
    status=1
    tail -40 "$work.log"
  fi
  grep -E "^stat::(number_of_executed_units|average_exec_per_sec|new_units_added|peak_rss_mb)|cov:" "$work.log" \
    | tail -5
  echo "corpus: $(ls "$work" | wc -l) inputs"
  rm -rf "$work" "$work.log"
done
exit $status
