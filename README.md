# qperf
A performance measurement tool for QUIC similar to iperf.
Uses https://github.com/h2o/quicly 

This fork adds `-P` (parallel streams over one connection), `-u` (upload mode) and
`--recv-window`. It was used to measure whether parallel QUIC streams increase
throughput — they do not, while parallel TCP connections do.

See [studies/](studies/) for the measurement studies run with it: each carries its
report, the raw data behind it, the scripts that regenerate its tables, and a runbook
to recreate it from scratch. The first one is
[parallel-streams-vs-connections](studies/parallel-streams-vs-connections/).

# basic usage and example output
```
Usage: ./qperf [options]

Options:
  -c target            run as client and connect to target server
  --cc [reno,cubic]    congestion control algorithm to use (default reno)
  -e                   measure time for connection establishment and first byte only
  -g                   enable UDP generic segmentation offload
  --iw initial-window  initial window to use (default 10)
  -l log-file          file to log tls secrets
  -p                   port to listen on/connect to (default 18080)
  -P streams           number of parallel streams to request over the single
                       connection (default 1, max 100). All streams share one
                       congestion window; the per-second report shows each
                       stream plus the total.
  --recv-window bytes  how much data the server may send us before waiting for
                       our acknowledgements -- a window on data in flight, NOT a
                       cap on total bytes transferred. Throughput cannot exceed
                       this divided by the round-trip time, so raise it on long
                       fat links. Client-side only; this is the QUIC
                       initial_max_data transport parameter. Accepts K/M/G
                       suffixes (default 16M)
  -s  address          listen as server on address
  -t time (s)          run for X seconds (default 10s)
  -u                   upload instead of download: the client sends and the
                       SERVER measures and reports throughput. Default is
                       download (server sends, client reports). Upload streams
                       are unidirectional and require a server built with
                       upload support.
  -h                   print this help
```

server
```
./qperf -s
starting server with pid 5624 on port 18080
got new connection
request received, sending data
connection 0 second 0 send window: 1112923 packets sent: 364792 packets lost: 373
connection 0 second 1 send window: 1238055 packets sent: 377515 packets lost: 123
connection 0 second 2 send window: 583352 packets sent: 355482 packets lost: 862
connection 0 second 3 send window: 275563 packets sent: 367538 packets lost: 607
connection 0 second 4 send window: 1100261 packets sent: 366005 packets lost: 20
connection 0 second 5 send window: 633010 packets sent: 356021 packets lost: 857
connection 0 second 6 send window: 1266610 packets sent: 367866 packets lost: 0
connection 0 second 7 send window: 1668530 packets sent: 360649 packets lost: 0
connection 0 second 8 send window: 1994930 packets sent: 364087 packets lost: 0
connection 0 second 9 send window: 1779683 packets sent: 374804 packets lost: 80
connection 0 total packets sent: 3654759 total packets lost: 2922
```
*Note*: The server looks for a TLS certificate and key in the current working dir named "server.crt" and "server.key" respectively([See TLS](#TLS)). You can use a self signed certificate; the client doesn't validate it.


client
```
./qperf -c 127.0.0.1
running client with host=127.0.0.1 and runtime=10s
connection establishment time: 6ms
time to first byte: 7ms
second 0: 3.144 gbit/s (422030372 bytes received)
second 1: 3.444 gbit/s (462189378 bytes received)
second 2: 3.184 gbit/s (427337822 bytes received)
second 3: 3.333 gbit/s (447304096 bytes received)
second 4: 2.996 gbit/s (402100242 bytes received)
second 5: 3.274 gbit/s (439462608 bytes received)
second 6: 3.083 gbit/s (413746021 bytes received)
second 7: 3.336 gbit/s (447686682 bytes received)
second 8: 3.034 gbit/s (407235597 bytes received)
second 9: 3.02 gbit/s (405314061 bytes received)
```

# parallel streams

`-P` asks the server for N streams over the *same* QUIC connection — one UDP
socket, one handshake, one congestion window shared by all of them. This is the
QUIC property that distinguishes it from running N TCP connections: N streams do
not get you N times the congestion window, so the aggregate should land close to
what a single stream achieves. The per-second report keeps the usual total line
and adds a breakdown per stream.
```
./qperf -c 127.0.0.1 -P 4 -t 3
starting client with host 127.0.0.1, port 18080, runtime 3s, cc reno, iw 10, streams 4, recv-window 16777216 bytes
connection establishment time: 8ms
time to first byte: 8ms
second 0: 2.055 gbit/s (275874787 bytes received)
  stream 0: 526.2 mbit/s (68967557 bytes received)
  stream 1: 526.2 mbit/s (68967161 bytes received)
  stream 2: 526.2 mbit/s (68970056 bytes received)
  stream 3: 526.2 mbit/s (68970013 bytes received)
```
The server is unchanged by `-P` — it serves whatever streams the client opens, and
still reports its send window once per *connection*, not once per stream. Streams
are scheduled round-robin by quicly, which is why the split above is even.

The ceiling is 100 streams: the server advertises `max_concurrent_streams_bidi=100`
and qperf streams never close, so it never issues more stream credit. The client
rejects `-P` above that rather than opening streams that would silently stall.

# upload

By default the server sends and the client measures. `-u` reverses that: the client
sends and the **server** prints the throughput, because the receiver is the side that
can measure goodput. Read the numbers on the server's console:
```
# client
./qperf -c 127.0.0.1 -u -P 2 -t 4
time to first ack: 8ms
second 0: 1.672 gbit/s (224354491 bytes acked)

# server -- the authoritative measurement
upload started, receiving
upload second 0: 1.689 gbit/s (226728893 bytes received)
  stream 0: 864.9 mbit/s (113362465 bytes received)
  stream 1: 864.9 mbit/s (113366428 bytes received)
```
The client's line reports *acknowledged* bytes rather than bytes handed to quicly, so
it excludes anything still in flight. It is a cross-check on the server's figure, not
a substitute: the two should agree within about a percent.

An upload stream is simply a **unidirectional** stream. Client-initiated uni streams
only ever flow client→server, so the stream type is itself the signal and there is no
request token for the server to parse.

**Both ends must be new enough.** The server has to advertise unidirectional stream
credit (`max_streams_uni`), which quicly's spec default sets to 0. Against an older
qperf server, `-u` will hang rather than fail, because the streams sit blocked waiting
for credit that never arrives.

# flow control

`--recv-window` sets how much data the server may send before it has to wait for
the client's acknowledgements. It is a window on data *in flight* — not a limit on
how many bytes the run will transfer. Throughput cannot exceed
`recv-window / round-trip-time`, so on a long fat link the default 16M can bind
before congestion control does, and a high-bandwidth high-latency test wants a
bigger value:
```
./qperf -c far-away-host -P 4 --recv-window 256M
```
It is the QUIC `initial_max_data` transport parameter, advertised by the client to
the server, so it is a client-side flag only. It is shared across all streams of the
connection, which means it applies to the aggregate of a `-P` run rather than to
each stream. (The per-stream windows are already effectively unlimited.)

# how to build

## Linux
### 1. Install required dependencies 
```
sudo apt update
sudo apt install git cmake libssl-dev libev-dev g++ -y
```
### 2.  
```
git clone --recurse-submodules https://github.com/rbruenig/qperf.git
mkdir build-qperf
cd build-qperf
cmake ../qperf
make
```

## macOS

Tested on Apple Silicon (Darwin 25.x) with Homebrew, CMake 4.4 and OpenSSL 3.

### 1. Install required dependencies
```
xcode-select --install
brew install cmake openssl@3 libev
```
`xcode-select --install` provides the Command Line Tools (clang and the system
headers); skip it if you already have Xcode installed. It requires no Apple
developer account.

Homebrew's OpenSSL is keg-only, so it is not on the default include/library search
path — the cmake invocation below points at it explicitly. Apple's system LibreSSL
will not work; picotls needs real OpenSSL 3.

### 2. Configure and build
```
git clone --recurse-submodules https://github.com/rbruenig/qperf.git
cd qperf

OPENSSL_PREFIX="$(brew --prefix openssl@3)"
LIBEV_PREFIX="$(brew --prefix libev)"

cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
  -DOPENSSL_ROOT_DIR="$OPENSSL_PREFIX" \
  -DOPENSSL_INCLUDE_DIR="$OPENSSL_PREFIX/include" \
  -DOPENSSL_SSL_LIBRARY="$OPENSSL_PREFIX/lib/libssl.dylib" \
  -DOPENSSL_CRYPTO_LIBRARY="$OPENSSL_PREFIX/lib/libcrypto.dylib" \
  -DCMAKE_C_FLAGS="-I$OPENSSL_PREFIX/include -I$LIBEV_PREFIX/include" \
  -DCMAKE_EXE_LINKER_FLAGS="-L$OPENSSL_PREFIX/lib -L$LIBEV_PREFIX/lib"

cmake --build build --parallel "$(sysctl -n hw.logicalcpu)"
```
The binary lands at `build/qperf`.

`-DCMAKE_POLICY_VERSION_MINIMUM=3.5` is required because the vendored quicly
declares `cmake_minimum_required(VERSION 2.8.12)`, and CMake 4.x refuses
compatibility with anything below 3.5.

### 3. macOS caveats
* **No UDP GSO.** `-g` relies on the `UDP_SEGMENT` socket option, which is
  Linux-only. On macOS `enable_gso()` prints a warning and falls back to one
  `sendmsg` per datagram, so `-g` is a no-op rather than a build failure.
* Throughput will be lower than on Linux for the same link, partly for the reason
  above and partly because macOS UDP socket buffers are small by default.

# TLS
QUIC requires TLS, so qperf requires TLS certificates when running in server mode. It will look for a "server.crt" and "server.key" file in the current working directory.

You can create these files by creating a self-signed certificate via openssl with the following one-liner:
```
openssl req -x509 -newkey rsa:4096 -keyout server.key -out server.crt -sha256 -days 365 -nodes -subj "/C=US/ST=Oregon/L=Portland/O=Company Name/OU=Org/CN=www.example.com"
```
