# Reproducing the multi-stream experiment

How to recreate [`multistream-report.md`](multistream-report.md) from scratch on any
Linux server, using **this fork** (upstream `rbruenig/qperf` has neither `-P` nor `-u`
and will not work).

You need two machines: a **server** (Linux) and a **client** (macOS or Linux). Both
run the same build of this repo. The interesting direction is **upload**, so the
client should sit on the link whose uplink you want to characterise.

## 1. Server

```bash
# THIS fork, not rbruenig/qperf. https so no SSH key is needed.
git clone --recurse-submodules https://github.com/njoubert/qperf.git
cd qperf

sudo apt update && sudo apt install -y git cmake libssl-dev libev-dev g++ iperf3
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel "$(nproc)"
```

If the clone predates this work, `git log --oneline | grep 'parallel streams'` should
find commit `8028251`. If it does not, you have upstream and `-u` will hang.

Certificates — qperf reads `server.crt`/`server.key` **from the current working
directory**, so generate them wherever you launch it. Self-signed is fine; the client
does not validate:

```bash
cd build
openssl req -x509 -newkey rsa:4096 -keyout server.key -out server.crt -sha256 \
  -days 365 -nodes -subj "/C=US/ST=Oregon/L=Portland/O=Company Name/OU=Org/CN=www.example.com"
```

Start both servers. **`--cc` here only affects the download direction**, because the
server is the sender only when downloading:

```bash
./qperf --cc cubic -s <server-ip> -p 5201     # UDP 5201
iperf3 -s -p 5201                             # TCP 5201  (systemd: iperf3.service)
```

They coexist: different protocols, same port number. **Except for UDP tests — see
gotchas.**

Firewall/NAT must forward **both** TCP 5201 (iperf3) **and** UDP 5201 (qperf, and
iperf3's UDP data flow).

## 2. Client

Same clone and build (see [README.md](README.md) for the macOS build, which needs
explicit OpenSSL paths), plus `brew install iperf3` or `apt install iperf3`.

## 3. Run the sweeps

Each takes ~4 minutes. `HOST` is the server as the client reaches it.

```bash
HOST=your-server.example.com
OUT=data/$(date +%F)-$HOST

# QUIC, N streams on ONE connection
scripts/sweep-qperf.sh  $HOST 5201 download $OUT/qperf-download-cubic
scripts/sweep-qperf.sh  $HOST 5201 upload   $OUT/qperf-upload

# TCP baseline, N SEPARATE connections -- the comparison that matters
scripts/sweep-iperf3.sh $HOST 5201 download $OUT/iperf3-download
scripts/sweep-iperf3.sh $HOST 5201 upload   $OUT/iperf3-upload
```

**The upload sweep's real numbers are printed by the server**, not the client — the
receiver is the only side that can measure goodput. Capture the server's stdout for
the whole sweep and save it as `$OUT/qperf-upload/server-authoritative.log`; the
analyzer expects all 12 runs in that one file, delimited by the server's
`upload started` / `upload finished` lines. The client's log reports acked bytes and
is only a cross-check (expect agreement within a few percent — if not, something is
wrong).

Then the path characterisation, which explains *why* the results look the way they do:

```bash
# STOP the qperf server first -- see gotchas
scripts/sweep-udp.sh $HOST 5201 $OUT/iperf3-udp
```

## 4. Analyze

```bash
scripts/analyze.py $OUT
```

Prints every table in the report, plus the headline scaling factors. Run it against
`data/2026-07-16-weshootfilm` to reproduce the original numbers exactly.

## Gotchas

Each of these cost real time to find, and most fail *silently*:

- **`--cc` applies to whichever side is SENDING, which flips with direction.**
  Download: the server sends, so the server's flag governs. Upload: the client sends,
  so the client's flag governs (`sweep-qperf.sh` passes it). The client always prints
  its own `cc` in its banner even when that value is irrelevant.
- **`-u` requires this fork on BOTH ends.** The server must advertise
  `max_streams_uni`; quicly's spec default is 0, so against an unpatched server the
  client's upload streams block forever waiting for credit — **a hang, not an error**.
- **Stop the qperf server before UDP tests.** iperf3's control channel is TCP 5201 but
  its UDP data targets UDP 5201, which qperf holds. Every UDP run then fails with
  `unable to read from stream socket: Resource temporarily unavailable` while the
  server log shows the control connection being accepted normally — the error points
  everywhere except the cause.
- **`-P` caps at 100.** The peer advertises `max_streams_bidi=100` and qperf streams
  never close, so credit is never reissued. Streams past 100 would stall silently;
  the client rejects them instead.
- **qperf prints mebibit/s but labels it `mbit/s`** (`format_size` divides by 1024).
  `analyze.py` recomputes from byte counts and reports SI. Do not compare qperf's
  printed rate against iperf3's directly — there is a ~5% gap.
- **Measure steady state.** A single 15s run on a lossy path may never converge; the
  metric window is seconds 5–14, and one run per configuration is worthless. Keep the
  interleaved ordering (`for rep: for N`) so no transient biases one configuration.
- **UDP sweeps are antisocial.** No congestion control, so they flood the link and the
  loss they report is partly self-inflicted. Only point them at links you own.

## What to expect

On a path whose uplink is lossy above some rate (as residential fiber often is):

- **QUIC streams: flat.** N=1 and N=8 within noise. One connection, one congestion
  window, streams dividing it.
- **TCP connections: scales, upstream.** N independent windows each tolerate the loss
  rate, so they aggregate.
- **Download may show nothing at all** if one flow already saturates the path. If TCP
  is also flat, the test cannot distinguish window-sharing from a link ceiling — check
  the UDP loss curve to find out which you are looking at.
