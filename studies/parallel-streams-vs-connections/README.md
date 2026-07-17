# Do parallel QUIC streams increase throughput?

**Date:** 2026-07-16
**Tool:** `qperf` (fork `njoubert/qperf`, branch `macos-build-support`), vendored quicly
**Baseline:** iperf3 3.21 (TCP)
**Question:** When N streams share one QUIC connection, do they share one congestion
window — so that aggregate throughput is independent of N?

## Summary

**Yes. N parallel QUIC streams over one connection give no throughput gain, while N
parallel TCP connections give up to 4.9x on the same path, in the same direction, at
the same time of day.** Streams share one congestion window; connections do not.

The decisive evidence is in the **upload** direction, where a single flow is severely
window-limited and parallelism therefore has room to pay off:

| N | qperf QUIC (N streams, 1 conn) | iperf3 TCP (N connections) |
|---|---|---|
| 1 | 50.5 | 59.8 |
| 2 | 43.9 | 108.3 |
| 4 | 48.5 | 192.5 |
| 8 | **46.8** | **294.5** |

*(SI Mbit/s, median of 3 runs, steady-state.)*

**Scaling factor from N=1 to N=8:**

| | upload | download |
|---|---|---|
| QUIC streams (1 connection) | **0.93x** | 1.01x |
| TCP connections | **4.92x** | 1.06x |

The path demonstrably carries ~295 Mbit/s upstream, and QUIC reaches only ~47 of it
no matter how many streams are used. There is no ceiling to hide behind: **the
bandwidth is provably available, and one congestion window cannot reach it.**

**The mechanism is loss.** A UDP sweep (no congestion control) shows the uplink is
clean to ~100 Mbit/s (~0.1% loss) and then degrades steeply — 2.57% at 150M, 5.49% at
200M, 8.65% at 300M — while still *delivering* 271 Mbit/s. A loss-based controller
derives its window from that loss rate, which pins any single flow to the tens of
Mbit/s: exactly where TCP N=1 (60–67) and QUIC N=1 (50) land. Eight TCP connections
each tolerate that loss independently and aggregate to ~295; eight QUIC streams share
the one clamped window and stay at ~47. See
[Mechanism](#mechanism--the-uplink-is-lossy-far-below-its-capacity).

Download, measured first, could not settle the question — there TCP is *also* flat
(+6%), because a single download flow already saturates the path. A flat QUIC result
in a direction where TCP is flat too proves nothing. Upload is where the effect
lives, and it required teaching qperf to upload at all.

## Direction matters, and download is the boring one

Both endpoints sit on **symmetric residential fiber**, so the asymmetry below is not
provisioning — it is congestion-window behaviour:

- **Download**: 1 → 8 TCP connections buys **+6%** (193 → 205 Mbit/s). A single flow
  already reaches ~193 against a ~200 Mbit/s ceiling. Nothing left to win.
- **Upload**: 1 → 8 TCP connections buys **+392%** (60 → 295 Mbit/s). A single flow
  captures barely a fifth of what the path carries.

Original qperf was **download-only by construction**: the server manufactured payload
(`server_stream_send_emit` → `memset(dst, 0x58, *len)` to `target_offset = UINT64_MAX`)
while the client sent 19 bytes (`"qperf start sending"`) and called `egress_shutdown`.
The client counted `bytes_received`; the server counted `packets sent`. So the tool
could only measure the direction where the phenomenon is invisible. Adding `-u` was a
prerequisite for answering the question, not a nice-to-have.

## Implementation under test

### `-P N` — parallel streams (client-side only)

Opens N bidirectional streams over a single QUIC connection: one UDP socket, one
handshake, one congestion window, round-robin scheduled by quicly's default
scheduler. No server change was needed — qperf's protocol was already per-stream and
the server serves whatever it is asked to open.

**Capped at 100.** The peer advertises `max_streams_bidi=100` (quicly's spec default)
and qperf streams never close, so credit is never reissued. Streams past 100 would
stall **silently and indefinitely** rather than error, so the client rejects `-P > 100`.

### `-u` — upload mode (a real protocol change)

An upload stream is simply a **unidirectional** stream. Client-initiated uni streams
only flow client→server, so the stream type is itself the signal — no request token
to parse, no ambiguity against the existing `"qperf start sending"` + FIN handshake.
Both `client_on_stream_open` and `server_on_stream_open` branch on
`quicly_stream_is_unidirectional`.

**The server must advertise `max_streams_uni`.** quicly's spec default is **0**, so
against an unpatched server the client's upload streams sit blocked forever waiting
for credit that never arrives — a hang, not an error. `-u` therefore requires a
rebuilt server, unlike `-P`. (The struct member is `max_streams_uni`; quicly's own
`defaults.c` comment calls it `max_concurrent_streams_uni`, which is not the field
name.)

**The server reports upload throughput**, because the receiver is the only side that
can measure goodput. The client reports *acknowledged* bytes (via `send_shift`) as an
independent cross-check — not emitted bytes, which would count data still in flight.

### `--recv-window`

Exposes the QUIC `initial_max_data` transport parameter: the connection-wide
flow-control window, a bound on data **in flight**, not on total bytes transferred.
It caps throughput at `window / RTT`, and is shared across all streams of a connection.

### Gotcha: `--cc` applies to the sender, which changes with direction

`main.c` hands `--cc` to whichever side you started. **For download the sender is the
server, so the server's flag governs. For upload the sender is the client, so the
client's flag governs.** The client's startup line prints its own `cc` value
regardless, which is misleading in download mode. A first upload probe was
accidentally run with the client on reno; caught before the sweep.

## Methodology

### Environment

| | |
|---|---|
| Client | macOS (Darwin 25.5.0), Apple Silicon, 192.168.1.122, Release build |
| Server | `weshootfilm.com` (192.184.170.71, internal 10.1.0.4), Linux 6.8, port 5201 |
| Path | Public internet, symmetric residential fiber both ends, ~19–30ms RTT |
| qperf download sender | server: `./qperf -s 10.1.0.4 -p 5201` (reno, then `--cc cubic`) |
| qperf upload sender | client: `./qperf -c ... -u --cc cubic` |
| iperf3 | server `iperf3 -s -p 5201`; sender CC cubic (download), unknown (upload) |
| Initial window | 10 packets (qperf default) |
| Flow-control window | 16 MiB (qperf default) |

qperf binds **UDP** 5201 while iperf3 listens on **TCP** 5201, so both servers ran
concurrently for the TCP and QUIC sweeps; the idle one carried no traffic.

**This coexistence does not extend to iperf3's UDP tests.** iperf3's control channel
is TCP 5201 but its UDP data flow targets UDP 5201 — the port qperf holds. With the
qperf server running, every UDP test failed with `unable to read from stream socket:
Resource temporarily unavailable` while the journal showed the control connection
being accepted normally. The qperf server has to be stopped before running UDP tests
on the same port.

The iperf3 server runs as a systemd unit (`iperf3.service`), so its receiver-side view
of every run is in `journalctl -u iperf3` — used below as an independent check on the
client's numbers.

The patched qperf was deployed to the server on a `multistream` branch (its tree was
a clean checkout of upstream `rbruenig/qperf` at 5ae5354, which is this fork's base,
so the branch diff applied cleanly) and rebuilt there with GCC on Linux.

The server host was **not under experimental control**: its hardware, load, and
competing traffic are unknown and unmeasured.

### Validation

Loopback, before any network measurement:

- **Stream count served correctly**: `-P 100` produced exactly 100 per-stream report
  lines, download and upload alike.
- **Scheduling is fair**: a 4-stream loopback run split 526.2 mbit/s per stream, even
  to four significant figures.
- **Aggregate flat in N on loopback**: 2.181 / 2.28 / 2.237 gbit/s at N = 1 / 4 / 100.
- **`--recv-window` provably reaches the server**: sweeping it down forces a monotonic
  collapse — 1024 B → ~333 mbit/s, 8 KiB → ~1.32 gbit/s, 64 KiB → ~2.13 gbit/s. It
  only binds below ~8 KiB on loopback, where RTT is small enough that the 16 MiB
  default permits ~4 gbit/s, far above what the CPU sustains.

**Upload measurement cross-check.** The client's acked-byte counter and the server's
received-byte counter are independent, at opposite ends of the connection, and agree:

| | loopback | real path (probe) |
|---|---|---|
| client acked | 1.672 gbit/s | 231.3 / 87.43 / 60.74 Mbit/s |
| server received | 1.689 gbit/s | 219.8 / 87.67 / 60.50 Mbit/s |

Agreement to ~1% on loopback and ~5% on the first second across the internet (where
acks for the final in-flight bytes land after the interval closes).

**TCP baseline reproduction.** The iperf3 upload figures were re-run after all the
qperf work, and reproduce — the key scaling result is not an artifact of when it was
measured:

| | original sweep | re-run | server journal (receiver-side) |
|---|---|---|---|
| N=1 | 59.8 | 66.7 | 66.7 |
| N=8 | 294.5 | 294 | 294 |

The server's own accounting (`journalctl -u iperf3`) matches the client's to the
decimal, and an earlier journal line — `[SUM] 0.00-15.02 sec 536 MBytes 300 Mbits/sec
receiver` — independently corroborates the N=8 upload sweep from the receiving end.
N=1 landed at the upper end of its original 51.9–76.2 spread on the re-run, which is
within that run-to-run variance rather than a change in behaviour.

Flow control was **not a factor** in any internet result: at ~25ms RTT the 16 MiB
default allows ~4 gbit/s, an order of magnitude above anything measured.

### Procedure

A pilot of one 10-second run per stream count produced an apparent 2x drop at N=4
that proved to be a single congestion event, not a stream-count effect. That
motivated the design used for all five sweeps:

- **4 configurations:** N = 1, 2, 4, 8
- **3 repetitions each**, 15 seconds per run, 2s gap
- **Interleaved ordering** — `for rep: for N in 1 2 4 8` — so each configuration is
  sampled across the whole measurement window and no transient can systematically
  bias one of them
- **12 runs per sweep**, ~4 minutes each

Five sweeps: qperf/reno download, qperf/cubic download, qperf/cubic **upload**,
iperf3 download (`-R`), iperf3 upload.

Plus two follow-ups: a **TCP reproduction** (N=1 and N=8 upload, cross-checked against
the server's journal) and a **UDP loss sweep** upstream at 50/100/150/200/300/400
Mbit/s targets, 10s each, single stream — the latter to characterise the path itself
rather than any transport's response to it.

### Metric

**Steady-state throughput** = mean of per-second totals over **seconds 5–14** of each
15-second run, discarding the first 5 seconds to exclude slow-start. For iperf3 this
is `sum.bits_per_second` per interval from its JSON. For qperf upload it is parsed
from the **server's** log, split into runs on its `upload started` / `upload finished`
markers.

Two reproduction details:

- The window is fixed at seconds 5–14 because some qperf runs emit a trailing
  `second 15: 0 bit/s` line at connection close. An earlier analysis used
  `second >= 5` unbounded and averaged that zero in for the one run that had it,
  understating it (77.9 vs 85.7). Medians and conclusions were unaffected.
- **All figures here are SI Mbit/s** (`bytes*8/1e6`). qperf's own console output is
  not: `format_size` divides by 1024 while labelling `mbit/s`, so what it prints is
  mebibit/s, ~5% below these numbers. iperf3's JSON is natively SI.

## Results

All values SI Mbit/s, steady-state (seconds 5–14), median of 3 reps.

### UPLOAD — the decisive comparison

**qperf — QUIC, N streams over ONE connection (measured by the server):**

| N | rep1 | rep2 | rep3 | **median** | spread |
|---|---|---|---|---|---|
| 1 | 41.5 | 50.5 | 57.6 | **50.5** | 41.5–57.6 |
| 2 | 40.7 | 47.1 | 43.9 | **43.9** | 40.7–47.1 |
| 4 | 48.5 | 56.2 | 42.6 | **48.5** | 42.6–56.2 |
| 8 | 46.8 | 58.1 | 44.7 | **46.8** | 44.7–58.1 |

**iperf3 — TCP, N separate connections:**

| N | rep1 | rep2 | rep3 | **median** | spread |
|---|---|---|---|---|---|
| 1 | 76.2 | 51.9 | 59.8 | **59.8** | 51.9–76.2 |
| 2 | 108.3 | 106.6 | 113.3 | **108.3** | 106.6–113.3 |
| 4 | 199.8 | 192.5 | 164.6 | **192.5** | 164.6–199.8 |
| 8 | 294.5 | 205.8 | 299.1 | **294.5** | 205.8–299.1 |

**QUIC is flat (0.93x from N=1 to N=8, i.e. no change within noise). TCP scales 4.92x
and climbs monotonically, nearly linearly to N=4.** Every QUIC median falls inside
every other QUIC median's spread; TCP's N=8 spread (205.8–299.1) does not even
overlap its N=1 spread (51.9–76.2).

TCP upload retransmits rise with N — the flows collectively provoke far more loss and
still move 5x the data, the classic parallel-TCP bargain:

| N | retransmits per run |
|---|---|
| 1 | 262, 95, 1413 |
| 2 | 229, 514, 1976 |
| 4 | 3252, 4138, 2910 |
| 8 | 7247, 2806, 4411 |

### DOWNLOAD — where the question cannot be settled

**qperf QUIC, cubic sender:**

| N | rep1 | rep2 | rep3 | **median** |
|---|---|---|---|---|
| 1 | 140.3 | 158.3 | 151.3 | **151.3** |
| 2 | 159.2 | 135.5 | 157.6 | **157.6** |
| 4 | 160.4 | 164.3 | 152.4 | **160.4** |
| 8 | 169.4 | 152.3 | 151.1 | **152.3** |

**qperf QUIC, reno sender:**

| N | rep1 | rep2 | rep3 | **median** |
|---|---|---|---|---|
| 1 | 150.5 | 154.7 | 151.0 | **151.0** |
| 2 | 168.4 | 109.0 | 146.7 | **146.7** |
| 4 | 89.9 | 153.3 | 153.3 | **153.3** |
| 8 | 145.5 | 169.6 | 159.1 | **159.1** |

**iperf3 TCP (`-R`):**

| N | rep1 | rep2 | rep3 | **median** |
|---|---|---|---|---|
| 1 | 193.1 | 193.2 | 126.5 | **193.1** |
| 2 | 108.4 | 154.2 | 125.5 | **125.5** |
| 4 | 180.5 | 150.5 | 192.2 | **180.5** |
| 8 | 204.6 | 206.0 | 200.5 | **204.6** |

QUIC is flat here too, but so is TCP (+6%), so this direction cannot distinguish
"streams share a window" from "the link was saturated anyway". TCP's N=2 median even
sits *below* its N=1 — its download "trend" rests on N=8 alone.

**Changing the qperf sender from reno to cubic barely moved anything** (151.0 → 151.3
at N=1; 159.1 → 152.3 at N=8). qperf was **not reno-limited**, which refutes the
hypothesis that congestion control explained the qperf-vs-iperf3 download gap — the
rerun was designed specifically to test that.

### Direction asymmetry (iperf3 TCP)

| N | upload | download | up/down |
|---|---|---|---|
| 1 | 59.8 | 193.1 | 0.31x |
| 2 | 108.3 | 125.5 | 0.86x |
| 4 | 192.5 | 180.5 | 1.07x |
| 8 | **294.5** | 204.6 | **1.44x** |

### MECHANISM — the uplink is lossy far below its capacity

A UDP sweep (no congestion control at all, so the sender simply blasts at the target
rate) maps what the path does under load. 10s per rate, 1 stream, upstream:

| target | sent | received | loss | lost/total | jitter |
|---|---|---|---|---|---|
| 50M | 50.0M | 49.9M | **0.12%** | 52/43185 | 0.32ms |
| 100M | 100.0M | 99.9M | **0.11%** | 96/86353 | 0.24ms |
| 150M | 150.0M | 146.1M | **2.57%** | 3334/129491 | 0.10ms |
| 200M | 200.0M | 189.0M | **5.49%** | 9484/172654 | 0.08ms |
| 300M | 296.7M | 271.0M | **8.65%** | 22175/256253 | 0.09ms |
| 400M | 400.0M | 326.7M | **18.33%** | 63329/345478 | 0.03ms |

Two facts, and together they explain every result above.

**1. The capacity is unambiguously there.** UDP delivers 271 Mbit/s upstream at a 300M
target, and ~327 Mbit/s at 400M. So a single QUIC stream sitting at 47 Mbit/s is
leaving roughly **5x of demonstrated, reachable bandwidth unused**. No link ceiling
explains that — this retires the last alternative to the congestion-window account.

**2. There is a loss knee between 100M and 150M.** Below ~100 Mbit/s the path is
essentially clean (~0.1%, background). Past that it degrades steeply but smoothly —
0.11% → 2.57% → 5.49% → 8.65% — the signature of a buffer beginning to overflow
rather than a hard policer. Jitter *falls* as the rate rises (0.32ms → 0.03ms),
consistent with a bottleneck that drops rather than queues.

That knee is the whole story of the single-flow limit. A loss-based controller (reno,
cubic) derives its equilibrium window from the loss rate it observes; at ~2.5% loss
the classic square-root law puts a single flow in the tens of Mbit/s — precisely where
TCP N=1 (60–67) and QUIC N=1 (50) land. **Neither is malfunctioning: both are doing
exactly what a loss-based controller does on a path with this loss profile.**

And this is why parallelism pays upstream. Eight TCP connections each independently
tolerate that loss rate, so eight windows aggregate to ~295 Mbit/s. QUIC's eight
streams share the *single* window the loss rate has already clamped to ~47. **The 6x
gap is not QUIC being slow — it is one window versus eight, against a lossy uplink.**

The download direction has no such knee within its range, which is the mirror
explanation for why a single download flow reaches ~193 Mbit/s while a single upload
flow cannot: downstream, one loss-based window is enough to fill the path.

## Interpretation

**The hypothesis is confirmed.** N QUIC streams multiplex over one connection with
one congestion controller, and the scheduler divides that window among them. Streams
buy head-of-line-blocking independence, not bandwidth. N TCP connections bring N
independent congestion windows, and where a single window is the binding constraint,
that is worth ~5x.

**Why upload was necessary.** Whether parallelism pays depends entirely on whether a
single flow is window-limited:

- Downstream, one flow already reaches ~193 against a ~200 Mbit/s ceiling. Extra
  windows have nothing to win — 8 TCP connections add ~6%, mostly by *reliably*
  filling the pipe (N=8 spread 200.5–206.0, the tightest result measured) rather than
  raising the ceiling. QUIC's flatness there is unfalsifiable.
- Upstream, one flow gets ~50–60 while eight get ~295. The single congestion window
  is the whole story, and QUIC — which can only ever have one — stays at ~47.

**The 6x gap at N=8 is the headline, and it is a property of the transport's
architecture, not of an implementation detail.** QUIC's own N=1 (50.5) is in the same
regime as TCP's N=1 (59.8); the divergence appears only when TCP is allowed to open
more windows and QUIC is not.

**The UDP sweep supplies the missing half of the explanation**: the uplink is clean to
~100 Mbit/s and then loses 2.5–8.7% while still delivering up to 271 Mbit/s. That loss
profile is what sets a single loss-based window to ~50–67 Mbit/s, and the available
headroom above it is what N independent windows convert into ~295. Both transports are
behaving correctly; the difference is purely how many windows each can bring to a
lossy path.

**This is a statement about one connection, not about QUIC's ceiling.** N QUIC
*connections* would presumably scale much as N TCP connections do. The measured
result is narrower and more precise: multiplexing streams onto one connection does
not multiply throughput, by design.

## Limitations

- **Upload CC parity is assumed, not proven.** The qperf client sender ran `--cc
  cubic`. macOS does not expose its TCP congestion control (`sysctl` has no such key)
  and iperf3 could not report it either — `sender_tcp_congestion: None` on upload,
  since that field is Linux-only. If macOS's TCP used something other than cubic, part
  of the N=1 gap (50.5 vs 59.8) is that rather than QUIC. **This does not touch the
  central finding**, which is the *scaling factor within each transport* (0.93x vs
  4.92x), where each tool is its own control.
- **Three repetitions establish "no large effect", not the absence of a small one.**
  A real ~5% stream-count advantage sits inside this noise floor.
- **Cross-tool runs were not simultaneous** — minutes to hours apart on a path with
  unmeasured cross-traffic. The qperf and iperf3 upload sweeps are the closest pair.
- **The server is uncontrolled**: load and competing traffic unknown, a plausible
  source of outliers (qperf/reno download N=4 rep1 collapsed 229 → 32 Mbit/s mid-run;
  iperf3 download N=1 rep3 managed only 126.5).
- **N=8 was the largest configuration**, and TCP upload had not obviously plateaued at
  294.5 — the true parallel ceiling is unmeasured.
- **Single path, single client pair, single day.** Results characterize this link.
- **Upload reporting state in `server_stream.c` is file-static**, so concurrent
  uploading connections would merge into one report. Fine for these sequential runs.
- **The UDP sweep is antisocial by design and partly self-inflicted.** With no
  congestion control it blasts a lossy link, so "the path carries 271 Mbit/s" means
  "while discarding 8.65% of what it is fed", not "cleanly". It maps the loss curve;
  it does not describe a rate any well-behaved flow should target.
- **The loss curve was measured upstream only.** The mirror claim — that download has
  no comparable knee below ~200 Mbit/s, which is why one download flow suffices — is
  inferred from the TCP/QUIC download results, not directly measured with UDP.
- **UDP loss was measured at a single point in time**, in one 10s run per rate, with no
  repetitions. The knee's location (between 100M and 150M) is therefore approximate.

## Reproduction

Full runbook — server setup, both tools, every gotcha — is in
[REPRODUCING.md](REPRODUCING.md). The short version, from this directory:

```bash
# sweeps (each ~4 min). --cc follows the SENDER, which flips with direction:
# download -> server sends (start the server with --cc); upload -> client sends.
scripts/sweep-qperf.sh  $HOST 5201 download data/<date>-<host>/qperf-download-cubic
scripts/sweep-qperf.sh  $HOST 5201 upload   data/<date>-<host>/qperf-upload
scripts/sweep-iperf3.sh $HOST 5201 download data/<date>-<host>/iperf3-download
scripts/sweep-iperf3.sh $HOST 5201 upload   data/<date>-<host>/iperf3-upload
scripts/sweep-udp.sh    $HOST 5201 data/<date>-<host>/iperf3-udp   # stop qperf first

# every table above, regenerated from the committed data:
scripts/analyze.py data/2026-07-16-weshootfilm
```

The upload sweep's authoritative numbers come from the **server's** stdout, saved as
`qperf-upload/server-authoritative.log`; the client's log is an acked-byte
cross-check. See [the data README](data/2026-07-16-weshootfilm/README.md) for how to
read the raw files.

## Next steps

- **Run the UDP sweep downstream** (`-u -R`), to confirm directly that download has no
  loss knee below ~200 Mbit/s. That is currently the one inferred link in the causal
  chain, and it is a 5-minute test.
- **Try a loss-tolerant congestion controller.** The whole single-flow limit follows
  from a loss-based window on a lossy path, so BBR — which models bandwidth rather
  than treating loss as congestion — should partly close the N=1 gap. quicly ships
  reno and cubic only, so this needs either a new `init_cc` or a different QUIC stack.
  This is the highest-value follow-up: it tests the mechanism, not just the outcome.
- **Push N past 8 on upload**, where TCP had not plateaued at 294.5 Mbit/s and the
  QUIC/TCP gap is still widening. `-P` supports up to 100.
- **Pin the upload CC parity down** — run the TCP baseline from a Linux client whose
  `sender_tcp_congestion` iperf3 can actually report, closing the one open confound.
- **Compare N QUIC *connections* against N TCP connections**, isolating "one window vs
  N windows" from "QUIC vs TCP". The prediction is that QUIC then scales like TCP.
- **More repetitions (~10) and longer runs (60s)** if effects below ~10% matter.
- **Test where the download path is not saturated** (a slower or more distant peer),
  so download can also distinguish window-sharing from a link ceiling.
