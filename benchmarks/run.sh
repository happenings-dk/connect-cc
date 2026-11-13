#!/usr/bin/env bash
# One fresh client process/channel per row; servers persist for the whole run.
set -euo pipefail
binary="${1:?Pass the absolute path to connectcpp_benchmark}"
output="${2:?Pass a new output directory}"
repetitions="${REPETITIONS:-3}"
total="${REQUESTS:-5000}"
warmup="${WARMUP:-100}"
for value in "$repetitions" "$total" "$warmup"; do
  [[ "$value" =~ ^[0-9]+$ ]] || { echo 'Expected integer settings' >&2; exit 2; }
done
((repetitions >= 1 && repetitions <= 100 && total >= 16 && total <= 2000000 && warmup <= 10000)) || exit 2
[[ "$binary" = /* && -x "$binary" ]] || { echo 'Use an absolute executable path' >&2; exit 2; }
if command -v timeout >/dev/null; then
  timeout_command=timeout
elif command -v gtimeout >/dev/null; then
  timeout_command=gtimeout
else
  echo 'GNU timeout is required (coreutils on macOS)' >&2
  exit 2
fi
mkdir -p "$(dirname "$output")"
mkdir "$output"
server_pids=()
cleanup() {
  for pid in "${server_pids[@]}"; do kill "$pid" 2>/dev/null || true; done
  for pid in "${server_pids[@]}"; do wait "$pid" 2>/dev/null || true; done
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
{
  date -u '+%Y-%m-%dT%H:%M:%SZ'
  uname -a
  echo "binary=$binary repetitions=$repetitions requests=$total warmup=$warmup"
  if command -v lscpu >/dev/null; then lscpu; fi
  if [[ -r /sys/fs/cgroup/cpu.max ]]; then cat /sys/fs/cgroup/cpu.max; fi
  if [[ -r /sys/fs/cgroup/cpuset.cpus.effective ]]; then cat /sys/fs/cgroup/cpuset.cpus.effective; fi
} > "$output/environment.txt"
if [[ -r /sys/fs/cgroup/cpu.stat ]]; then cp /sys/fs/cgroup/cpu.stat "$output/cpu-before.txt"; fi
for server in connectcpp native; do
  "$binary" server "$server" > "$output/$server.port" 2> "$output/$server.log" &
  server_pids+=("$!")
  for ((attempt = 0; attempt < 100; ++attempt)); do
    [[ -s "$output/$server.port" ]] && break
    kill -0 "$!" 2>/dev/null || { cat "$output/$server.log" >&2; exit 1; }
    sleep 0.1
  done
  [[ -s "$output/$server.port" ]] || { echo 'Server startup timed out' >&2; exit 1; }
done
printf '%s\n' 'repeat,server,client,payload_bytes,concurrency,iterations_per_lane,warmup_per_lane,completed,errors,elapsed_seconds,rpc_per_second,p50_us,p95_us,p99_us' > "$output/results.csv"
pairs=('connectcpp connect' 'connectcpp grpc' 'connectcpp native' 'native grpc' 'native native')
for ((repeat = 1; repeat <= repetitions; ++repeat)); do
  for bytes in 0 1024 65536; do
    for concurrency in 1 16; do
      iterations=$((total / concurrency))
      # Rotate pairing order between repeats to reduce fixed-order bias.
      for ((offset = 0; offset < ${#pairs[@]}; ++offset)); do
        index=$(((offset + repeat - 1) % ${#pairs[@]}))
        read -r server client <<< "${pairs[index]}"
        read -r port < "$output/$server.port"
        echo "repeat=$repeat server=$server client=$client bytes=$bytes concurrency=$concurrency" >&2
        "$timeout_command" 180 "$binary" client "$client" "$port" "$bytes" \
          "$concurrency" "$iterations" "$warmup" > "$output/current.csv" 2>> "$output/client.log"
        printf '%s,%s,%s\n' "$repeat" "$server" "$(cat "$output/current.csv")" >> "$output/results.csv"
      done
    done
  done
done
rm "$output/current.csv"
if [[ -r /sys/fs/cgroup/cpu.stat ]]; then cp /sys/fs/cgroup/cpu.stat "$output/cpu-after.txt"; fi
