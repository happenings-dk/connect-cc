# Unary RPC benchmarks

The harness compares Connect for C++'s public typed unary API with native gRPC C++
1.84.0. All 90 measured scenarios completed **449,640 RPCs with zero errors**;
response bytes and completion counts are checked. These are local loopback
measurements, not a claim about every workload or a protocol verification gate.

## Reproduce

Use the pinned checkout and Linux toolchain in [DEPENDENCIES.md](DEPENDENCIES.md).
From the repository root, with `VCPKG_ROOT` set:

```sh
ci/run-benchmarks.sh "$PWD/build/benchmark-results"
```

The output directory must be new. The script builds the optional benchmark
feature and records source/binary hashes, dependency versions, compiler settings,
raw CSV rows and CPU information. For the same four-CPU container allocation:

```sh
docker build -f ci/Dockerfile -t connectcpp-ci .
docker run --rm --cpus 4 --memory 10g --pids-limit 1024 \
  --cap-drop ALL --security-opt no-new-privileges \
  --mount "type=bind,src=$PWD,dst=/workspace" \
  connectcpp-ci ci/run-benchmarks.sh /workspace/build/benchmark-results
```

The harness, command-line options and measurement boundaries are documented in
[benchmarks/README.md](../benchmarks/README.md). GitHub CI is configured for a
short correctness smoke run. The [initial hosted run](https://github.com/happenings-dk/connect-cc/actions/runs/43515948558)
passed all 30 scenarios; a later cold dependency build timed out before RPC
measurement and is being rerun with caching. See [hosted verification](HOSTED_VERIFICATION.md).
Equivalent smoke runs passed locally on Linux GCC 14 and macOS Apple Clang, including a
macOS ASan/UBSan build.

## Recorded environment and workload

The run began at 00:14:46 UTC on 14 November 2026 at runtime revision `72d2739` on an AMD Ryzen 9 9950X Linux
x86-64 host. Client and server ran as separate processes in one Ubuntu 24.04
container, sharing a four-CPU quota and 10 GiB memory limit. GCC 14.2 compiled
Release builds (`-O3 -DNDEBUG`). The dependency baseline was
`3d22b64585ca026e881728fd87e541c23a611a43`, including gRPC 1.84.0 and Protobuf
7.36.2. No additional CPU throttling was recorded during measurement. The host
was not dedicated or CPU-pinned; other workloads and frequency scaling remain
possible sources of variation. Package builds and a fuzz campaign were also
running on the host during this run; these measurements are a reproducible
workload snapshot, not an isolated performance baseline.

Each case uses protobuf binary, cleartext HTTP/2, unary echo, identity encoding,
no retries, a reused client connection/channel and a ten-second per-call
deadline. Payload sizes are application bytes in each direction, before protobuf
and HTTP framing. Concurrency is a closed-loop count of outstanding calls.
There are 100 warmup requests per lane, followed by 5,000 measured requests at
concurrency 1 or 4,992 at concurrency 16, repeated three times. Each repetition
rotates the client/server pairing order. Channel establishment, warmup, result
sorting and teardown are outside the measured interval.

Connect for C++ uses one application-owned Asio thread in each process. Native gRPC
uses a completion-queue client with one application polling thread and a
callback server, plus gRPC's internal worker threads. These thread models are
part of this comparison; the numbers do not isolate codec efficiency or compare
equal worker counts. TLS, streaming, JSON, WAN latency and open-loop overload
are outside this benchmark's scope.

## Results

Each cell below is the median of three independent runs. Throughput is completed
RPCs per second. “Connect” and “gRPC” columns use Connect for C++ at both ends with
the named protocol. “Native” uses native gRPC C++ at both ends.

| Payload bytes | Concurrency | Connect RPC/s | gRPC RPC/s | Native RPC/s |
| --- | --- | ---: | ---: | ---: |
| 0 | 1 | 24,998 | 16,633 | 18,597 |
| 0 | 16 | 53,204 | 39,807 | 64,520 |
| 1,024 | 1 | 22,342 | 18,340 | 17,268 |
| 1,024 | 16 | 48,971 | 38,383 | 60,735 |
| 65,536 | 1 | 7,292 | 6,789 | 8,970 |
| 65,536 | 16 | 15,345 | 14,081 | 26,708 |

The following are medians of each run's p99 latency, in microseconds. They are
not percentiles of pooled observations. Raw rows also contain p50 and p95.

| Payload bytes | Concurrency | Connect p99 µs | gRPC p99 µs | Native p99 µs |
| --- | --- | ---: | ---: | ---: |
| 0 | 1 | 89.1 | 133.8 | 90.8 |
| 0 | 16 | 327.8 | 512.3 | 408.5 |
| 1,024 | 1 | 102.7 | 129.7 | 107.8 |
| 1,024 | 16 | 461.6 | 580.8 | 426.7 |
| 65,536 | 1 | 246.3 | 260.3 | 181.7 |
| 65,536 | 16 | 1,366.2 | 1,728.9 | 1,024.5 |

Native gRPC delivered higher throughput at concurrency 16 for every tested
payload size. Connect for C++'s Connect mode delivered higher throughput for the
small concurrency-1 cases. This identifies workloads worth profiling without
establishing a general performance ranking or a release threshold.

The [complete CSV](benchmark-results/linux-gcc14.csv) also includes a native
client calling the C++ server and a connect-cc gRPC client calling the
native server. [Evidence files](benchmark-results/README.md) preserve all
repetitions and the execution metadata; the summary does not discard slower
runs or failures.
