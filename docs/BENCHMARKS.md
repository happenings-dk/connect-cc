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
short correctness smoke run; that hosted job has not been executed. Equivalent
smoke runs passed locally on Linux GCC 14 and macOS Apple Clang, including a
macOS ASan/UBSan build.

## Recorded environment and workload

The run began at 21:03:15 UTC on 13 November 2026 on an AMD Ryzen 9 9950X Linux
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
| 0 | 1 | 30,331 | 21,632 | 22,783 |
| 0 | 16 | 55,224 | 41,416 | 89,384 |
| 1,024 | 1 | 27,351 | 19,728 | 21,215 |
| 1,024 | 16 | 50,462 | 40,333 | 73,012 |
| 65,536 | 1 | 9,418 | 8,100 | 10,872 |
| 65,536 | 16 | 16,447 | 14,929 | 28,317 |

The following are medians of each run's p99 latency, in microseconds. They are
not percentiles of pooled observations. Raw rows also contain p50 and p95.

| Payload bytes | Concurrency | Connect p99 µs | gRPC p99 µs | Native p99 µs |
| --- | --- | ---: | ---: | ---: |
| 0 | 1 | 47.0 | 75.9 | 64.2 |
| 0 | 16 | 405.3 | 403.8 | 297.4 |
| 1,024 | 1 | 90.0 | 127.6 | 69.8 |
| 1,024 | 16 | 446.4 | 431.1 | 352.1 |
| 65,536 | 1 | 161.8 | 156.6 | 131.0 |
| 65,536 | 16 | 1,151.3 | 1,256.7 | 950.1 |

Native gRPC delivered higher throughput at concurrency 16 for every tested
payload size. Connectcpp's Connect mode delivered higher throughput for the
small concurrency-1 cases. This identifies workloads worth profiling without
establishing a general performance ranking or a release threshold.

The [complete CSV](benchmark-results/linux-gcc14.csv) also includes a native
client calling the Connectcpp server and a Connectcpp gRPC client calling the
native server. [Evidence files](benchmark-results/README.md) preserve all
repetitions and the execution metadata; the summary does not discard slower
runs or failures.
