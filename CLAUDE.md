# qperf — working notes

QUIC performance measurement tool (iperf-like), built on [quicly](https://github.com/h2o/quicly).
This is a **fork** of `rbruenig/qperf`, patched for local macOS use.

## Repo layout

- `main.c` — arg parsing, dispatch to client/server
- `client.c` / `client_stream.c`, `server.c` / `server_stream.c` — the two modes
- `common.c` / `common.h` — TLS ctx, address resolution, datagram send paths
- `extern/quicly` — vendored submodule (with its own `deps/picotls`, `klib`, `picotest`).
  Keep it clean; all our patches live in the top-level sources.

## Remotes

- `origin` → `git@github.com:njoubert/qperf.git` (the fork — **push here**)
- `upstream` → `https://github.com/rbruenig/qperf.git` (fetch only)

Upstream's *push* URL is deliberately set to an invalid value so `git push upstream`
fails loudly instead of prompting for credentials on someone else's repo. Fetching
from it is unaffected. Don't "fix" this.

## Build

Always `git clone --recurse-submodules` (or `git submodule update --init --recursive`).

### macOS (Apple Silicon, Homebrew)

```
xcode-select --install          # clang + system headers
brew install cmake openssl@3 libev

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

Binary: `build/qperf`. Two flags people delete without understanding:

- **The OpenSSL paths** are required because Homebrew's `openssl@3` is keg-only,
  so it isn't on the default search path. Apple's system LibreSSL will *not*
  substitute — picotls needs real OpenSSL 3.
- **`CMAKE_POLICY_VERSION_MINIMUM=3.5`** is required because vendored quicly
  declares `cmake_minimum_required(VERSION 2.8.12)`, and CMake 4.x refuses
  compatibility below 3.5.

### Linux

Plain `cmake -B build -DCMAKE_BUILD_TYPE=Release` after
`apt install git cmake libssl-dev libev-dev g++`. Nothing special.

## Our patches (branch `macos-build-support`)

Both are **preprocessor guards that only affect non-Linux builds** — load-bearing,
don't "simplify" them away:

- `common.h` — includes `pthread.h` under `#ifdef __APPLE__`. `get_current_pid()`
  already had an `__APPLE__` branch calling `pthread_threadid_np()` but never
  included the declaring header, so that path had apparently never been compiled.
- `common.c` — `enable_gso()` assigns `send_dgrams_gso` only under `#ifdef __linux__`.
  That function is itself already inside `#ifdef __linux__`, so the unconditional
  assignment failed to *link* on macOS. Now `-g` warns and falls back to the
  default one-`sendmsg`-per-datagram path.

### Verifying a patch doesn't disturb Linux

No Docker locally and CI only runs on master/PRs, so the cheap local check is to
resolve the conditionals as a Linux compiler would and diff against the previous
revision — identical output means the Linux token stream is untouched:

```
unifdef -D__linux__ -U__APPLE__ common.c > /tmp/patched.c
git show HEAD:common.c | unifdef -D__linux__ -U__APPLE__ > /tmp/orig.c
diff -B /tmp/orig.c /tmp/patched.c   # empty == provably inert on Linux
```

This is a preprocessor-level proof, not a real compile. For the real thing, open a
PR against the fork's master — `.github/workflows` builds on ubuntu-latest and
triggers on `pull_request` to master (a plain branch push does *not* trigger it).

## Running

```
./build/qperf -s              # server, default port 18080
./build/qperf -c HOST -p PORT # client, 10s run
```

Server reads `server.crt` / `server.key` **from the current working directory**;
self-signed is fine, the client doesn't validate. Generate per the README.
`test-certs/` is gitignored along with `server.key`/`server.crt` — never commit keys.

## Gotchas

- `-g` (UDP GSO) is a **Linux-only** no-op on macOS; it relies on the `UDP_SEGMENT`
  socket option. Don't compare macOS and Linux throughput naively — that, plus
  small default macOS UDP socket buffers, makes macOS numbers lower for the same link.
- CI installs only `libev-dev`, relying on the runner image for OpenSSL. Pre-existing
  and brittle, but not ours to fix unless it breaks.
</content>
