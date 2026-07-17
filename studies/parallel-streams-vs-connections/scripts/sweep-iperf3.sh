#!/usr/bin/env bash
# TCP baseline: N *separate connections*, the counterpart to qperf's N streams on
# one connection. This is the comparison the whole experiment turns on.
#
# Usage: sweep-iperf3.sh HOST PORT download|upload OUTDIR [REPS] [STREAMS...]
#
# Direction: iperf3 defaults to client->server (upload). -R reverses it so the
# server sends, which is what matches qperf's default download.
set -euo pipefail

HOST=${1:?usage: sweep-iperf3.sh HOST PORT download|upload OUTDIR [REPS] [STREAMS...]}
PORT=${2:?missing PORT}
DIR=${3:?missing direction: download|upload}
OUT=${4:?missing OUTDIR}
REPS=${5:-3}
shift 5 2>/dev/null || shift $(($#))
STREAMS=("${@:-1 2 4 8}")
# a single arg may be a quoted list ("1 2 4 8"); split it. plain `[ ] && cmd` would
# abort the script under `set -e` when the test is false, hence the if.
if [ ${#STREAMS[@]} -eq 1 ]; then read -ra STREAMS <<< "${STREAMS[0]}"; fi

RUNTIME=${RUNTIME:-15}

case "$DIR" in
  download) DIRFLAG="-R" ;;
  upload)   DIRFLAG="" ;;
  *) echo "direction must be 'download' or 'upload'" >&2; exit 1 ;;
esac

command -v iperf3 >/dev/null || { echo "iperf3 not installed (brew/apt install iperf3)" >&2; exit 1; }
mkdir -p "$OUT"

echo "iperf3 TCP $DIR sweep -> $HOST:$PORT, ${REPS} reps x [${STREAMS[*]}] connections, ${RUNTIME}s each"

for REP in $(seq 1 "$REPS"); do
  for P in "${STREAMS[@]}"; do
    echo "  rep${REP} connections=${P}"
    # shellcheck disable=SC2086
    iperf3 -c "$HOST" -p "$PORT" -P "$P" -t "$RUNTIME" $DIRFLAG -J \
      > "$OUT/rep${REP}_streams${P}.json" 2>&1 || true
    sleep 2
  done
done

echo "done -> $OUT"
