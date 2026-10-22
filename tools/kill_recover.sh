#!/usr/bin/env bash
# Repeatedly write to a log, kill -9 the writer at a random moment, then check
# that every acknowledged write is recovered.
#
#   tools/kill_recover.sh <path-to-crash_demo> [iterations]
set -euo pipefail

demo=${1:?usage: kill_recover.sh <path-to-crash_demo> [iterations]}
iterations=${2:-100}
dir=$(mktemp -d)
trap 'rm -rf "$dir"' EXIT

for ((i = 1; i <= iterations; i++)); do
  rm -f "$dir/wal" "$dir/acks"
  "$demo" write "$dir/wal" >"$dir/acks" &
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
done
echo "all $iterations iterations passed"
