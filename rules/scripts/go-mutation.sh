#!/usr/bin/env bash
# Run gremlins mutation testing and enforce the configured efficacy score.
#
# Usage: go-mutation.sh <src_dir> <min_ratio>
set -euo pipefail

src_dir=$1
min_ratio=$2
log_file=$(mktemp)
trap 'rm -f "$log_file"' EXIT

(cd "$src_dir" && gremlins unleash .) | tee "$log_file"
required=$(awk -v value="$min_ratio" 'BEGIN { printf "%.2f", value * 100 }')
efficacy=$(grep -o 'Test efficacy: [0-9.]*' "$log_file" | tail -1 | awk '{print $3}')
if [[ -z "$efficacy" ]]; then
  # No mutable code is reported as "No results to report"; treat it as clean.
  if grep -q 'No results to report' "$log_file"; then
    efficiency=100
  else
    echo "mutation: gremlins did not report an efficacy score" >&2
    exit 2
  fi
else
  efficiency=$efficacy
fi
awk -v score="$efficiency" -v minimum="$required" 'BEGIN {
  printf "mutation efficacy: %.2f%%, minimum %.2f%%\n", score, minimum
  exit !(score + 0 >= minimum + 0)
}'
