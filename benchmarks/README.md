# RPC comparison harness

`main.cc` compares this library's public typed unary API with native gRPC C++.
`echo.proto` defines the common bytes echo service. `run.sh` runs five compatible
client/server pairs and writes per-run CSV measurements. gRPC is an optional
benchmark dependency, never a runtime dependency of connectcpp.

Configure the root project with `CONNECTCPP_BUILD_BENCHMARKS=ON` and vcpkg's
`benchmarks` feature. Build the `connectcpp_benchmark` target, then run:

```sh
benchmarks/run.sh "$PWD/build-benchmarks/benchmarks/connectcpp_benchmark" \
  "$PWD/build/benchmark-results"
```

For the pinned Linux GCC 14 toolchain, `ci/run-benchmarks.sh NEW_OUTPUT_DIR`
configures, builds, runs and records dependency/compiler/source information.
Set `VCPKG_ROOT` to the pinned checkout described in `docs/DEPENDENCIES.md`.
The GitHub benchmark workflow performs a short correctness smoke run, not a
performance gate. No throughput or latency threshold is imposed.

The output directory must not exist. The script needs Bash and GNU `timeout`
(`gtimeout` from Homebrew coreutils works on macOS). It starts only loopback
servers and terminates its own server processes on exit. No services or fixed
ports are required. Optional `REPETITIONS`, `REQUESTS` and `WARMUP` environment
variables default to 3 repetitions, approximately 5,000 measured requests per
row, and 100 warmup requests per concurrent lane. Requests are rounded down to
a multiple of concurrency. A failed RPC or incorrect echo fails the run.

The workload is cleartext HTTP/2, protobuf binary, unary POST, no compression,
no retries, one reused connection/channel per client process, payload sizes
0/1,024/65,536 bytes in each direction, and concurrency 1/16. Both servers copy
the decoded request into the response. Requests have a 10-second deadline.
Each row uses a fresh client process, warms up, waits for all warmup calls, and
then measures the next batch. Channel setup, warmup, sorting and teardown are
outside elapsed time. Per-call latency includes client message handling and
scheduling; throughput uses wall time from launching the batch until all calls
complete. Percentiles use nearest rank over successful calls. Response contents
are checked after the latency timestamp and before another call starts.

The Connectcpp transport adapter explicitly advertises only identity response
encoding; the native gRPC channel enables only its identity compression bit.
This avoids benchmarking compressed responses from a Connectcpp server against
uncompressed responses from the native server's default configuration.

Connectcpp uses one application-owned Asio thread in each process. Native gRPC
uses an asynchronous completion-queue client with one application polling
thread and a callback server; gRPC also owns internal worker threads. This is
an end-to-end comparison of those APIs, not equal thread counts or isolated
codec cost. Both processes share the machine/container CPU allocation. Other
host workloads, scheduling and container throttling can affect measurements.
No TLS, streaming, JSON, WAN, allocator tuning or overload behavior is measured.
Closed-loop concurrency does not describe latency under a fixed arrival rate.

`results.csv` contains every repetition; `environment.txt`, CPU quota counters
and logs describe the execution environment. Keep dependency versions, compiler
flags, source fingerprints and machine details alongside published results.
Do not pool latency percentiles across runs or treat a single run as a universal
performance claim. Native APIs follow the upstream [callback server tutorial](https://grpc.io/docs/languages/cpp/callback/)
and [completion-queue client tutorial](https://grpc.io/docs/languages/cpp/async/).
