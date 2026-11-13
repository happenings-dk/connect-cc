# Unary RPC benchmarks

The harness compares Connectcpp's public typed unary API with native gRPC C++
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
short correctness smoke run; all 30 hosted scenarios passed with zero errors. Equivalent
smoke runs passed locally on Linux GCC 14 and macOS Apple Clang, including a
macOS ASan/UBSan build.

## Recorded environment and workload

The run began at 22:58:56 UTC on 13 November 2026 on an AMD Ryzen 9 9950X Linux
x86-64 host. Client and server ran as separate processes in one Ubuntu 24.04
container, sharing a four-CPU quota and 10 GiB memory limit. GCC 14.2 compiled
Release builds (`-O3 -DNDEBUG`). The dependency baseline was
`3d22b64585ca026e881728fd87e541c23a611a43`, including gRPC 1.84.0 and Protobuf
7.36.2. No additional CPU throttling was recorded during measurement. The host
was not dedicated or CPU-pinned; other workloads and frequency scaling remain
possible sources of variation.

Each case uses protobuf binary, cleartext HTTP/2, unary echo, identity encoding,
no retries, a reused client connection/channel and a ten-second per-call
deadline. Payload sizes are application bytes in each direction, before protobuf
and HTTP framing. Concurrency is a closed-loop count of outstanding calls.
There are 100 warmup requests per lane, followed by 5,000 measured requests at
concurrency 1 or 4,992 at concurrency 16, repeated three times. Each repetition
rotates the client/server pairing order. Channel establishment, warmup, result
sorting and teardown are outside the measured interval.

Connectcpp uses one application-owned Asio thread in each process. Native gRPC
uses a completion-queue client with one application polling thread and a
callback server, plus gRPC's internal worker threads. These thread models are
part of this comparison; the numbers do not isolate codec efficiency or compare
equal worker counts. TLS, streaming, JSON, WAN latency and open-loop overload
are outside this benchmark's scope.

## Results

Each cell below is the median of three independent runs. Throughput is completed
RPCs per second. “Connect” and “gRPC” columns use Connectcpp at both ends with
the named protocol. “Native” uses native gRPC C++ at both ends.

| Payload bytes | Concurrency | Connect RPC/s | gRPC RPC/s | Native RPC/s |
| --- | --- | ---: | ---: | ---: |
| 0 | 1 | 27,884 | 20,176 | 21,498 |
| 0 | 16 | 52,004 | 40,530 | 70,141 |
| 1,024 | 1 | 26,678 | 18,032 | 20,571 |
| 1,024 | 16 | 48,443 | 38,818 | 66,800 |
| 65,536 | 1 | 8,674 | 7,364 | 10,179 |
| 65,536 | 16 | 15,839 | 14,808 | 27,310 |

The following are medians of each run's p99 latency, in microseconds. They are
not percentiles of pooled observations. Raw rows also contain p50 and p95.

| Payload bytes | Concurrency | Connect p99 µs | gRPC p99 µs | Native p99 µs |
| --- | --- | ---: | ---: | ---: |
| 0 | 1 | 85.9 | 120.2 | 66.0 |
| 0 | 16 | 428.2 | 531.9 | 375.7 |
| 1,024 | 1 | 71.9 | 136.2 | 86.1 |
| 1,024 | 16 | 507.4 | 500.6 | 381.8 |
| 65,536 | 1 | 196.7 | 274.2 | 144.1 |
| 65,536 | 16 | 1,241.3 | 1,423.6 | 1,046.6 |

Native gRPC delivered higher throughput at concurrency 16 for every tested
payload size. Connectcpp's Connect mode delivered higher throughput for the
small concurrency-1 cases. This identifies workloads worth profiling without
establishing a general performance ranking or a release threshold.

The [complete CSV](benchmark-results/linux-gcc14.csv) also includes a native
client calling the Connectcpp server and a Connectcpp gRPC client calling the
native server. [Evidence files](benchmark-results/README.md) preserve all
repetitions and the execution metadata; the summary does not discard slower
runs or failures.
