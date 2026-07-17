#!/usr/bin/env bash
# Sweep qperf over stream counts: 3 reps x N streams, interleaved.
#
# Interleaved ordering (for rep: for N) matters -- it stops a transient on the path
# from systematically biasing one stream count. Do not "optimise" it into
# for N: for rep.
#
# Usage: sweep-qperf.sh HOST PORT download|upload OUTDIR [REPS] [STREAMS...]
#
# Direction note: --cc applies to whichever side is SENDING.
#   download -> the SERVER sends, so start the server with --cc.
#   upload   -> the CLIENT sends, so --cc is passed here (and the server's own
#               --cc is irrelevant to the measurement).
#
# In upload mode the throughput printed here is only acked bytes, a cross-check.
# The authoritative number is on the server's stdout -- capture it there.
set -euo pipefail

HOST=${1:?usage: sweep-qperf.sh HOST PORT download|upload OUTDIR [REPS] [STREAMS...]}
PORT=${2:?missing PORT}
DIR=${3:?missing direction: download|upload}
OUT=${4:?missing OUTDIR}
REPS=${5:-3}
shift 5 2>/dev/null || shift $(($#))
STREAMS=("${@:-1 2 4 8}")
# a single arg may be a quoted list ("1 2 4 8"); split it. plain `[ ] && cmd` would
# abort the script under `set -e` when the test is false, hence the if.
if [ ${#STREAMS[@]} -eq 1 ]; then read -ra STREAMS <<< "${STREAMS[0]}"; fi

QPERF=${QPERF:-./build/qperf}
RUNTIME=${RUNTIME:-15}
CC=${CC:-cubic}

case "$DIR" in
  # PREFIX marks upload client logs as the cross-check they are; the server's log is
  # the authoritative one. Computed here, not inline in the filename: `var=$(cmd)`
  # takes the substitution's exit status, so a false test there aborts under `set -e`.
  download) DIRFLAG=""; PREFIX="" ;;
  upload)   DIRFLAG="-u --cc $CC"; PREFIX="client_" ;;
  *) echo "direction must be 'download' or 'upload'" >&2; exit 1 ;;
esac

[ -x "$QPERF" ] || { echo "no qperf binary at $QPERF (set QPERF=...)" >&2; exit 1; }
mkdir -p "$OUT"

echo "qperf $DIR sweep -> $HOST:$PORT, ${REPS} reps x [${STREAMS[*]}] streams, ${RUNTIME}s each"
if [ "$DIR" = upload ]; then
  echo "NOTE: authoritative numbers are on the SERVER's stdout; save it as server-authoritative.log"
fi

for REP in $(seq 1 "$REPS"); do
  for P in "${STREAMS[@]}"; do
    f="$OUT/${PREFIX}rep${REP}_streams${P}.log"
    echo "  rep${REP} streams=${P}"
    # shellcheck disable=SC2086
    "$QPERF" -c "$HOST" -p "$PORT" $DIRFLAG -P "$P" -t "$RUNTIME" > "$f" 2>&1 || true
    sleep 2
  done
done

echo "done -> $OUT"
