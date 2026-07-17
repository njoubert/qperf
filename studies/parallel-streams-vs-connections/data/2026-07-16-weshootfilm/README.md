# Raw data — 2026-07-16, weshootfilm.com

The measurements behind [the study report](../../README.md). Every number in that
report is derived from these files; nothing else was used.

Regenerate the report's tables with, from the study directory:

```
scripts/analyze.py data/2026-07-16-weshootfilm
```

## Provenance

| | |
|---|---|
| Client | macOS (Darwin 25.5.0), Apple Silicon, Release build |
| Server | `weshootfilm.com` (internal 10.1.0.4), Linux 6.8, port 5201 |
| Path | Public internet, symmetric residential fiber both ends, ~19–30ms RTT |
| qperf | this repo at `8028251` (parallel streams + upload mode) |
| iperf3 | 3.21, running as `iperf3.service` on the server |

Each sweep is 3 repetitions x N in {1,2,4,8} streams/connections, 15s per run,
interleaved (`for rep: for N`) so no configuration is systematically biased by a
transient. The UDP sweep is a single 10s run per target rate.

## Layout

| directory | what | sender | measured by |
|---|---|---|---|
| `qperf-download-reno/` | QUIC, N streams, 1 conn | server (reno) | client |
| `qperf-download-cubic/` | QUIC, N streams, 1 conn | server (cubic) | client |
| `qperf-upload/` | QUIC, N streams, 1 conn | client (cubic) | **server** |
| `iperf3-download/` | TCP, N connections (`-R`) | server (cubic) | client |
| `iperf3-upload/` | TCP, N connections | client (cc unverifiable) | client |
| `iperf3-udp/` | UDP, no congestion control | client | server |

## Reading these files

- **qperf download** logs are client stdout: one `second N:` line per second with the
  aggregate, plus indented `stream N:` lines when more than one stream.
- **qperf upload**: `client_*.log` reports *acked* bytes and is only a cross-check.
  The authoritative figure is `server-authoritative.log` — the receiver is the side
  that can measure goodput. That single file holds all 12 runs, delimited by
  `upload started, receiving` / `upload finished`, in the order
  rep1[N=1,2,4,8], rep2[...], rep3[...].
- **iperf3** files are `-J` JSON: per-second intervals under `intervals[].sum`,
  totals under `end`.

## Gotchas encoded in this data

- **qperf prints mebibit/s but labels it `mbit/s`** (`format_size` divides by 1024).
  The report converts to SI Mbit/s; iperf3's JSON is already SI. Do not mix them.
- **Some qperf runs emit a trailing `second 15: 0 bit/s`** line at connection close.
  The metric window is fixed at seconds 5–14 to exclude it; averaging it in
  understates that run.
- **`iperf3-upload/` has no `sender_tcp_congestion`** — the sender is macOS and that
  field is Linux-only. The upload CC parity is assumed, not proven. This is the one
  open confound in the report.
