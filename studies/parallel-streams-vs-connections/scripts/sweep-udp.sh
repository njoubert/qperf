#!/usr/bin/env bash
# UDP loss sweep: characterises the PATH, not any transport's response to it.
# No congestion control, so this just blasts at each target rate and reports what
# fraction survives. That loss curve is what explains why a single congestion
# window stalls far below the path's capacity.
#
# Usage: sweep-udp.sh HOST PORT OUTDIR [RATES_Mbit...]
#
# !! STOP ANY qperf SERVER ON THIS PORT FIRST !!
# iperf3's control channel is TCP, but its UDP data flow targets UDP <PORT> -- the
# same port a qperf server binds. With qperf running, every UDP test fails with
#   "unable to read from stream socket: Resource temporarily unavailable"
# while the server journal happily shows the control connection being accepted.
# The error points at everything except the real cause.
#
# This is deliberately antisocial traffic: it floods a link with no backoff.
# Keep runs short and do not point it at anything you do not own.
set -euo pipefail

HOST=${1:?usage: sweep-udp.sh HOST PORT OUTDIR [RATES_Mbit...]}
PORT=${2:?missing PORT}
OUT=${3:?missing OUTDIR}
shift 3
RATES=("${@:-50 100 150 200 300 400}")
# a single arg may be a quoted list ("50 100 200"); split it. plain `[ ] && cmd` would
# abort the script under `set -e` when the test is false, hence the if.
if [ ${#RATES[@]} -eq 1 ]; then read -ra RATES <<< "${RATES[0]}"; fi

RUNTIME=${RUNTIME:-10}
REVERSE=${REVERSE:-}   # set REVERSE=-R to measure the download direction instead

command -v iperf3 >/dev/null || { echo "iperf3 not installed (brew/apt install iperf3)" >&2; exit 1; }
mkdir -p "$OUT"

echo "iperf3 UDP sweep -> $HOST:$PORT, rates [${RATES[*]}] Mbit/s, ${RUNTIME}s each"
echo "(if every run errors, a qperf server is probably still holding UDP $PORT)"

for B in "${RATES[@]}"; do
  echo "  ${B} Mbit/s"
  # shellcheck disable=SC2086
  iperf3 -c "$HOST" -p "$PORT" -u -b "${B}M" -t "$RUNTIME" $REVERSE -J \
    > "$OUT/upload_${B}Mbit.json" 2>&1 || true
  sleep 3
done

echo "done -> $OUT"
