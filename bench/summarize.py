#!/usr/bin/env python3
"""Turn a bench/run.sh summary.txt into markdown tables (median of rounds per server).

usage: python3 bench/summarize.py bench/results/<run>/summary.txt
"""
import re
import statistics
import sys
from collections import defaultdict

LINE = re.compile(
    r"^(?P<scenario>\S+)\s+(?P<server>\S+)\s+r(?P<round>\d+)\s+(?P<rps>[\d.]+) req/s\s+p50 (?P<p50>\S+)\s+"
    r"p99 (?P<p99>\S+)(?:\s+p99\.9 (?P<p999>\S+)\s+p99\.99 (?P<p9999>\S+))?\s+cpu srv (?P<scpu>\d+)% cli (?P<ccpu>\d+)%"
    r"(?P<errors>.*)$")

TITLES = {
    "hello": "In-memory 13-byte response (`/hello`), keep-alive",
    "file1k": "1 KiB static file, keep-alive",
    "file16k": "16 KiB static file, keep-alive",
    "file1m": "1 MiB static file, keep-alive (sendfile)",
    "pipeline": "`/hello`, 16 pipelined requests per round trip",
    "close": "`/hello`, new connection per request (`Connection: close`)",
    "latency": "Latency at a fixed rate (wrk2, coordinated-omission corrected)",
}


def to_ms(v):
    if v is None:
        return None
    m = re.match(r"([\d.]+)(us|ms|s)$", v)
    if not m:
        return None
    x = float(m.group(1))
    return {"us": x / 1000, "ms": x, "s": x * 1000}[m.group(2)]


def fmt_ms(x):
    if x is None:
        return "-"
    return "%.0f µs" % (x * 1000) if x < 1 else "%.2f ms" % x


def main(path):
    rows = defaultdict(lambda: defaultdict(list))
    header = []
    for line in open(path):
        if line.startswith("#"):
            header.append(line.strip())
            continue
        m = LINE.match(line.strip())
        if not m:
            continue
        rows[m["scenario"]][m["server"]].append(m.groupdict())
    for h in header:
        print("    " + h)
    print()
    for scenario, servers in rows.items():
        print("### " + TITLES.get(scenario, scenario))
        print()
        lat = scenario == "latency"
        if lat:
            print("| server | achieved req/s | p50 | p99 | p99.9 | p99.99 | server CPU |")
            print("|---|---:|---:|---:|---:|---:|---:|")
        else:
            print("| server | req/s (median) | vs nginx | rounds | p50 | p99 | server CPU | wrk CPU |")
            print("|---|---:|---:|---|---:|---:|---:|---:|")
        nginx = None
        if "nginx" in servers:
            nginx = statistics.median(float(r["rps"]) for r in servers["nginx"])
        for server in ("hws", "nginx", "old"):
            rs = servers.get(server)
            if not rs:
                continue
            rps = [float(r["rps"]) for r in rs]
            med = statistics.median(rps)
            mid = min(rs, key=lambda r: abs(float(r["rps"]) - med))
            name = {"old": "previous server", "hws": "**hws**"}.get(server, server)
            def col(key):  # per-column median across rounds
                vals = [to_ms(r[key]) for r in rs if to_ms(r[key]) is not None]
                return statistics.median(vals) if vals else None

            if lat:
                print("| %s | %s | %s | %s | %s | %s | %.0f%% |" % (
                    name, "{:,.0f}".format(med), fmt_ms(col("p50")), fmt_ms(col("p99")),
                    fmt_ms(col("p999")), fmt_ms(col("p9999")), statistics.median(int(r["scpu"]) for r in rs)))
            else:
                ratio = "%.0f%%" % (med / nginx * 100) if nginx else "-"
                # wrk times only one request per pipelined batch, so its percentiles are meaningless there.
                p50 = "n/a" if scenario == "pipeline" else fmt_ms(to_ms(mid["p50"]))
                p99 = "n/a" if scenario == "pipeline" else fmt_ms(to_ms(mid["p99"]))
                print("| %s | %s | %s | %s | %s | %s | %s%% | %s%% |" % (
                    name, "{:,.0f}".format(med), ratio, ", ".join("{:,.0f}".format(x) for x in rps),
                    p50, p99, mid["scpu"], mid["ccpu"]))
            errs = {r["errors"].strip() for r in rs if r["errors"].strip()}
            for e in errs:
                print("|  | ⚠ %s | | | | | | |" % e)
        print()


if __name__ == "__main__":
    main(sys.argv[1])
