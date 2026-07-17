#!/usr/bin/env python3
"""Regenerate the report's tables from a raw data directory.

Usage: scripts/analyze.py data/2026-07-16-weshootfilm

Metric: mean throughput over seconds 5-14 of each run, reported in SI Mbit/s.

Two things this handles that hand-rolled parsing gets wrong:

  * qperf prints mebibit/s while labelling it "mbit/s" (format_size divides by
    1024), so its printed rate is ~5% below SI. We ignore the printed rate and
    recompute from the byte counts. iperf3's JSON is already SI.

  * Some qperf runs emit a trailing "second 15: 0 bit/s" line at connection close.
    The 5..14 window excludes it; averaging it in silently understates that run.
"""
import json
import re
import statistics
import sys
from pathlib import Path

LO, HI = 5, 14  # steady-state window: skip slow-start, exclude the close artifact
STREAMS = (1, 2, 4, 8)
REPS = (1, 2, 3)


def qperf_client(path):
    """Mean SI Mbit/s from a qperf client log (aggregate 'second N:' lines)."""
    vals = []
    for line in path.read_text().splitlines():
        m = re.match(r"second (\d+): .*?\((\d+) bytes (?:received|acked)\)", line)
        if m and LO <= int(m.group(1)) <= HI:
            vals.append(int(m.group(2)))
    return sum(vals) * 8 / len(vals) / 1e6 if vals else None


def qperf_upload_server(path):
    """Split the server's upload log into runs and return one mean per run.

    Runs are delimited by 'upload started' / 'upload finished' and appear in the
    order rep1[N=1,2,4,8], rep2[...], rep3[...].
    """
    runs, cur = [], None
    for line in path.read_text().splitlines():
        if "upload started" in line:
            cur = []
        elif "upload finished" in line and cur is not None:
            runs.append(cur)
            cur = None
        elif cur is not None:
            m = re.match(r"upload second (\d+): .*?\((\d+) bytes received\)", line)
            if m and LO <= int(m.group(1)) <= HI:
                cur.append(int(m.group(2)))
    return [sum(r) * 8 / len(r) / 1e6 if r else None for r in runs]


def iperf3_tcp(path):
    d = json.loads(path.read_text())
    if "error" in d:
        return None
    iv = [i["sum"]["bits_per_second"] / 1e6
          for i in d["intervals"] if LO <= i["sum"]["start"] < HI + 1]
    return statistics.mean(iv) if iv else None


def table(title, per_stream):
    """per_stream: {N: [rep1, rep2, rep3]} -> print medians and spread."""
    print(f"\n{title}")
    print(f"{'':6}{'rep1':>9}{'rep2':>9}{'rep3':>9}{'median':>10}{'spread':>17}")
    medians = {}
    for n in STREAMS:
        v = [x for x in per_stream.get(n, []) if x is not None]
        if not v:
            print(f"N={n:<5}{'(no data)':>9}")
            continue
        medians[n] = statistics.median(v)
        cells = "".join(f"{x:>9.1f}" for x in v)
        print(f"N={n:<5}{cells}{medians[n]:>10.1f}{min(v):>9.1f}-{max(v):<7.1f}")
    if len(medians) >= 2 and STREAMS[0] in medians and STREAMS[-1] in medians:
        lo, hi = medians[STREAMS[0]], medians[STREAMS[-1]]
        print(f"{'':6}scaling N={STREAMS[0]} -> N={STREAMS[-1]}: {hi / lo:.2f}x")
    return medians


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    root = Path(sys.argv[1])
    if not root.is_dir():
        sys.exit(f"no such directory: {root}")

    print(f"Steady-state throughput, seconds {LO}-{HI}, SI Mbit/s")
    summary = {}

    for name, sub in (("qperf QUIC download, reno sender  (N streams, 1 conn)", "qperf-download-reno"),
                      ("qperf QUIC download, cubic sender (N streams, 1 conn)", "qperf-download-cubic")):
        d = root / sub
        if d.is_dir():
            summary[sub] = table(name, {
                n: [qperf_client(d / f"rep{r}_streams{n}.log")
                    for r in REPS if (d / f"rep{r}_streams{n}.log").exists()]
                for n in STREAMS})

    # upload: the server is authoritative; its single log holds all 12 runs in order
    srv = root / "qperf-upload" / "server-authoritative.log"
    if srv.exists():
        runs = qperf_upload_server(srv)
        order = [(r, n) for r in REPS for n in STREAMS]
        per = {}
        for (r, n), v in zip(order, runs):
            per.setdefault(n, []).append(v)
        summary["qperf-upload"] = table(
            "qperf QUIC UPLOAD, cubic sender (N streams, 1 conn) -- SERVER-MEASURED", per)
        if len(runs) != len(order):
            print(f"  !! expected {len(order)} runs in the server log, found {len(runs)}")

    for name, sub in (("iperf3 TCP download (N separate connections)", "iperf3-download"),
                      ("iperf3 TCP UPLOAD   (N separate connections)", "iperf3-upload")):
        d = root / sub
        if d.is_dir():
            summary[sub] = table(name, {
                n: [iperf3_tcp(d / f"rep{r}_streams{n}.json")
                    for r in REPS if (d / f"rep{r}_streams{n}.json").exists()]
                for n in STREAMS})

    # the mechanism: what the path does with no congestion control at all
    udp = root / "iperf3-udp"
    if udp.is_dir():
        print("\niperf3 UDP loss sweep (no congestion control) -- characterises the PATH")
        print(f"{'target':>8}{'sent':>9}{'received':>10}{'loss':>9}{'jitter':>10}")
        for f in sorted(udp.glob("upload_*Mbit.json"),
                        key=lambda p: int(re.search(r"(\d+)Mbit", p.name).group(1))):
            d = json.loads(f.read_text())
            if "error" in d:
                print(f"{f.name:>8}  ERROR: {d['error'][:45]}")
                continue
            s = d["end"]["sum"]
            sent = d["end"].get("sum_sent", s)["bits_per_second"] / 1e6
            recv = s["bits_per_second"] / 1e6 * (1 - s["lost_percent"] / 100)
            tgt = re.search(r"(\d+)Mbit", f.name).group(1)
            print(f"{tgt:>6}M{sent:>8.1f}M{recv:>9.1f}M"
                  f"{s['lost_percent']:>8.2f}%{s['jitter_ms']:>8.2f}ms")

    # the headline: scaling within each transport, each its own control
    up_q = summary.get("qperf-upload", {})
    up_t = summary.get("iperf3-upload", {})
    if up_q and up_t:
        print("\n=== HEADLINE: upload scaling, N=1 -> N=8 ===")
        print(f"  QUIC streams on 1 connection: {up_q[8] / up_q[1]:.2f}x  "
              f"({up_q[1]:.1f} -> {up_q[8]:.1f} Mbit/s)")
        print(f"  TCP separate connections:     {up_t[8] / up_t[1]:.2f}x  "
              f"({up_t[1]:.1f} -> {up_t[8]:.1f} Mbit/s)")


if __name__ == "__main__":
    main()
