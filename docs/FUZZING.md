# Protocol fuzzing

`connectcpp_protocol_fuzz` drives bounded envelope and streaming parsers,
HTTP/1.1 requests/responses and pipelining, gRPC-Web base64/trailers, Connect
terminal/error JSON, GET decoding, gRPC timeout/status/details, content types,
Connect compression preferences, and unary identity/gzip responses. The first two input bytes select the parser,
protocol/flags and fragment size. Seeds in `fuzz/corpus` include valid frames,
terminal records, HTTP messages, metadata, GET parameters and concatenated gzip
members (including empty members).

Build with upstream Clang and its libFuzzer runtime. Enable ASan/UBSan too;
`fuzzer-no-link` instruments the library itself, not just the entry point.
Build dependencies with the same sanitizer configuration; see
[SANITIZERS.md](SANITIZERS.md). The Linux helper `ci/run-sanitizer.sh fuzz`
selects the pinned, instrumented target dependencies and ordinary host tools.

```sh
cmake -S . -B build-fuzz -G Ninja \
  -DCMAKE_CXX_COMPILER=clang++-18 -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DBUILD_TESTING=OFF -DCONNECTCPP_BUILD_GENERATOR=OFF \
  -DCONNECTCPP_BUILD_FUZZERS=ON -DCONNECTCPP_SANITIZE=ON
cmake --build build-fuzz --parallel 4
mkdir -p build-fuzz/corpus build-fuzz/artifacts
cp fuzz/corpus/* build-fuzz/corpus/
build-fuzz/connectcpp_protocol_fuzz build-fuzz/corpus \
  -max_total_time=120 -max_len=4096 -timeout=10 -rss_limit_mb=1024 \
  -artifact_prefix=build-fuzz/artifacts/
```

Add the vcpkg toolchain flags from DEPENDENCIES.md for the pinned dependency
baseline. CI retains the evolved corpus, run log, and any failure artifacts.
Reproduce a failure by passing its artifact file as the sole positional input.
Promote reproducible failures to regression tests before fixing them. Longer
campaigns can increase the time/input-length limits; the entry point rejects
inputs above 64 KiB and keeps message/decompression/metadata limits at 64 KiB.
The harness also aborts if a parser produces events indefinitely without
consuming input. This is a liveness check, not a throughput benchmark.

Fuzz configuration is separate from TSan. The CI smoke budget is finite and is
not a claim of exhaustive parser coverage or proof of protocol compliance.
CONFORMANCE.md tracks executed verification, distinct from configured jobs.

## Recorded verification

At runtime revision `72d2739`, Linux Clang 18 with ASan/UBSan completed
**7,099,792 inputs in 121 seconds** without a finding. The task used a two-CPU,
8-GiB container and the evolved corpus plus all checked-in seeds. Logs are
retained in `build/verification/tcp-shutdown/fuzz-final-audit.log` in the
development workspace. The corpus includes the concatenated-gzip and
identity-before-gzip regressions.

The [hosted campaign](https://github.com/happenings-dk/connect-cc/actions/runs/43524141112/job/132225569915)
at `a705cb6` completed 2,903,286 inputs in 121 seconds without a finding.
[Hosted verification](HOSTED_VERIFICATION.md) records this separately from
compiler/conformance checks. Earlier local campaign logs are retained by source
revision in the verification workspace.

These are finite campaigns with explicit input, memory and time limits. A run
without a finding does not establish exhaustive coverage or prove that a parser
is free of defects. Keep discovered failures as regression inputs and rerun the
relevant harness whenever parser behavior changes.
