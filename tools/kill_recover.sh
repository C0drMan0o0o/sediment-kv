#!/usr/bin/env bash
# Repeatedly write to a log, kill -9 the writer at a random moment, then check
# that every acknowledged write is recovered.
#
#   tools/kill_recover.sh <path-to-crash_demo> [iterations] [sync|nosync]
#
# In nosync mode the writer never blocks in fsync, so kills often land between
# a frame's header and payload writes. The run then also fails unless at least
# one iteration recovered past a torn tail, so the torn-tail path is exercised.
# kill -9 leaves the page cache intact, so acknowledged writes must still
# survive in either mode.
set -euo pipefail

demo=${1:?usage: kill_recover.sh <path-to-crash_demo> [iterations] [sync|nosync]}
iterations=${2:-100}
mode=${3:-sync}
case $mode in
  sync) write_args=() ;;
  nosync) write_args=(nosync) ;;
  *) echo "mode must be sync or nosync" >&2; exit 2 ;;
esac
torn=0
dir=$(mktemp -d)
trap 'rm -rf "$dir"' EXIT

for ((i = 1; i <= iterations; i++)); do
  rm -f "$dir/wal" "$dir/acks"
  "$demo" write "$dir/wal" ${write_args[@]+"${write_args[@]}"} >"$dir/acks" &
  pid=$!
  # Wait for the first acknowledgement so slow process start-up is not
  # mistaken for a crash.
  until [[ -s "$dir/acks" ]]; do sleep 0.01; done
  # Kill between 20 and 220 ms in, so the writer dies at varying points.
  sleep "0.$(printf '%03d' $((20 + RANDOM % 200)))"
  kill -9 "$pid"
  wait "$pid" 2>/dev/null || true

  # The last line may be cut short if the kill landed mid-print.
  acked=$(grep -c '^ack [0-9]*$' "$dir/acks" || true)
  if ! out=$("$demo" verify "$dir/wal" "$acked"); then
    echo "iteration $i FAILED (acked $acked)" >&2
    exit 1
  fi
  echo "iteration $i: $out"
  [[ $out != *"dropped 0 bytes"* ]] && torn=$((torn + 1))
done
if [[ $mode == nosync && $torn -eq 0 ]]; then
  echo "no torn tail seen in $iterations iterations; test did not exercise it" >&2
  exit 1
fi
echo "all $iterations iterations passed ($torn with a torn tail)"
