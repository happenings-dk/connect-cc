#!/usr/bin/env bash
set -euo pipefail
build_dir="${1:?Pass the conformance-enabled build directory}"
repetitions="${2:-20}"
if [[ ! "$repetitions" =~ ^[1-9][0-9]*$ ]]; then
  echo 'Repetitions must be a positive integer' >&2
  exit 2
fi
for ((iteration = 1; iteration <= repetitions; ++iteration)); do
  for profile in unary http1; do
    echo "Deadline repetition $iteration, $profile"
    "$build_dir/bin/connectconformance" --mode server \
      --conf "conformance/$profile.yaml" --max-servers 2 --run 'Timeouts/**' \
      -- "$build_dir/connectcpp_conformance" server
  done
done
